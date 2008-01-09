/*******************************************************************************
 * libproxy - A library for proxy configuration
 * Copyright (C) 2006 Nathaniel McCallum <nathaniel@natemccallum.com>
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 ******************************************************************************/

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <dlfcn.h>
#include <math.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>


#include "misc.h"
#include "proxy_factory.h"
#include "wpad.h"
#include "config_file.h"

#define DEFAULT_CONFIG_ORDER "USER,SESSION,SYSTEM"

struct _pxProxyFactoryConfig {
	pxConfigCategory           category;
	char                      *name;
	pxProxyFactoryPtrCallback  callback;
};
typedef struct _pxProxyFactoryConfig pxProxyFactoryConfig;

struct _pxKeyVal {
	char *key;
	void *value;
};
typedef struct _pxKeyVal pxKeyVal;

struct _pxProxyFactory {
	pthread_mutex_t             mutex;
	void                      **plugins;
	pxProxyFactoryConfig      **configs;
	pxKeyVal                  **misc;
	pxProxyFactoryVoidCallback *on_get_proxy;
	pxPACRunnerCallback         pac_runner;
	pxPAC                      *pac;
	pxWPAD                     *wpad;
	pxConfigFile               *cf;
};

// Convert the PAC formatted response into our proxy URL array response
static char **
_format_pac_response(char *response)
{
	char **chain, *tmp;
	
	if (!response) return px_strsplit("direct://", ";");
	chain = px_strsplit(response, ";");
	px_free(response);
	
	for (int i=0 ; chain[i] ; i++)
	{
		tmp = px_strstrip(chain[i]);
		px_free(chain[i]);
		
		if (!strncmp(tmp, "PROXY", 5) || !strncmp(tmp, "SOCKS", 5))
		{
			char *hostport = px_strstrip(tmp + 5);
			if (!strncmp(tmp, "PROXY", 5))
				chain[i] = px_strcat("http://", hostport, NULL);
			else
				chain[i] = px_strcat("socks://", hostport, NULL);
			px_free(hostport);
		}
		else
			chain[i] = px_strdup("direct://");
		
		px_free(tmp);
	}
	
	return chain;
}

static inline bool
_endswith(char *string, char *suffix)
{
	int st_len = strlen(string);
	int su_len = strlen(suffix);
	
	return (st_len >= su_len && !strcmp(string + (st_len-su_len), suffix));
}

static bool
_sockaddr_equals(const struct sockaddr *ip_a, const struct sockaddr *ip_b, const struct sockaddr *nm)
{
	if (!ip_a || !ip_b) return false;
	if (ip_a->sa_family != ip_b->sa_family) return false;
	if (nm && ip_a->sa_family != nm->sa_family) return false;
	
	// Setup the arrays
	uint8_t bytes = 0, *a_data = NULL, *b_data = NULL, *nm_data = NULL;
	if (ip_a->sa_family == AF_INET)
	{
		bytes   = 32 / 8;
		a_data  = (uint8_t *) &((struct sockaddr_in *) ip_a)->sin_addr;
		b_data  = (uint8_t *) &((struct sockaddr_in *) ip_b)->sin_addr;
		nm_data = nm ? (uint8_t *) &((struct sockaddr_in *) nm)->sin_addr : NULL;
	}
	else if (ip_a->sa_family == AF_INET6)
	{
		bytes   = 128 / 8;
		a_data  = (uint8_t *) &((struct sockaddr_in6 *) ip_a)->sin6_addr;
		b_data  = (uint8_t *) &((struct sockaddr_in6 *) ip_b)->sin6_addr;
		nm_data = nm ? (uint8_t *) &((struct sockaddr_in6 *) nm)->sin6_addr : NULL;
	}
	else
		return false;
	
	for (int i=0 ; i < bytes ; i++)
	{
		if (nm && (a_data[i] & nm_data[i]) != (b_data[i] & nm_data[i]))
			return false;
		else if (a_data[i] != b_data[i])
			return false;
	}
	return true;
}

static struct sockaddr *
_sockaddr_from_string(const char *ip, int len)
{
	if (!ip) return NULL;
	struct sockaddr *result = NULL;
	
	// Copy the string
	if (len >= 0)
		ip = px_strndup(ip, len);
	else
		ip = px_strdup(ip);
	
	// Try to parse IPv4
	result = px_malloc0(sizeof(struct sockaddr_in));
	result->sa_family = AF_INET;
	if (inet_pton(AF_INET, ip, &((struct sockaddr_in *) result)->sin_addr) > 0)
		goto out;
	
	// Try to parse IPv6
	px_free(result);
	result = px_malloc0(sizeof(struct sockaddr_in6));
	result->sa_family = AF_INET6;
	if (inet_pton(AF_INET6, ip, &((struct sockaddr_in6 *) result)->sin6_addr) > 0)
		goto out;
	
	// No address found
	px_free(result);
	result = NULL;
	out:
		px_free((char *) ip);
		return result;
}

static struct sockaddr *
_sockaddr_from_cidr(int af, int cidr)
{
	// TODO: Support CIDR notation
	return NULL;
}

static bool
_ip_ignore(pxURL *url, char *ignore)
{
	if (!url || !ignore) return false;
	
	bool result   = false;
	uint32_t port = 0;
	const struct sockaddr *dst_ip = px_url_get_ip_no_dns(url);
	      struct sockaddr *ign_ip = NULL, *net_ip = NULL;
	
	// IPv4
	// IPv6
	if ((ign_ip = _sockaddr_from_string(ignore, -1)))
		goto out;
	
	// IPv4/CIDR
	// IPv4/IPv4
	// IPv6/CIDR
	// IPv6/IPv6
	if (strchr(ignore, '/'))
	{
		ign_ip = _sockaddr_from_string(ignore, strchr(ignore, '/') - ignore);
		net_ip = _sockaddr_from_string(strchr(ignore, '/') + 1, -1);
		
		// If CIDR notation was used, get the netmask
		if (ign_ip && !net_ip)
		{
			uint32_t cidr = 0;
			if (sscanf(strchr(ignore, '/') + 1, "%d", &cidr) == 1)
				net_ip = _sockaddr_from_cidr(ign_ip->sa_family, cidr);
		}
		
		if (ign_ip && net_ip && ign_ip->sa_family == net_ip->sa_family)
			goto out;

		px_free(ign_ip);
		px_free(net_ip);
		ign_ip = NULL;
		net_ip = NULL;
	}
	
	// IPv4:port
	// [IPv6]:port
	if (strrchr(ignore, ':') && sscanf(strrchr(ignore, ':'), ":%u", &port) == 1 && port > 0)
	{
		ign_ip = _sockaddr_from_string(ignore, strrchr(ignore, ':') - ignore);
		
		// Make sure this really is just a port and not just an IPv6 address
		if (ign_ip && (ign_ip->sa_family != AF_INET6 || ignore[0] == '['))
			goto out;
		
		px_free(ign_ip);
		ign_ip = NULL;
		port   = 0;
	}
	
	out:
		result = _sockaddr_equals(dst_ip, ign_ip, net_ip);
		px_free(ign_ip);
		px_free(net_ip);
		return port != 0 ? (port == px_url_get_port(url) && result): result;
}

static inline bool
_domain_ignore(pxURL *url, char *ignore)
{
	if (!url || !ignore)
		return false;
	
	// Get our URL's hostname and port
	char *host = px_strdup(px_url_get_host(url));
	int   port = px_url_get_port(url);
	
	// Get our ignore pattern's hostname and port
	char *ihost = px_strdup(ignore);
	int   iport = 0;
	if (strchr(ihost, ':'))
	{
		char *tmp = strchr(ihost, ':');
		if (sscanf(tmp+1, "%d", &iport) == 1)
			*tmp  = '\0';
		else
			iport = 0;
	}
	
	// Hostname match (domain.com or domain.com:80)
	if (!strcmp(host, ihost))
		if (!iport || port == iport)
			goto match;
	
	// Endswith (.domain.com or .domain.com:80)
	if (ihost[0] == '.' && _endswith(host, ihost))
		if (!iport || port == iport)
			goto match;
	
	// Glob (*.domain.com or *.domain.com:80)
	if (ihost[0] == '*' && _endswith(host, ihost+1))
		if (!iport || port == iport)
			goto match;
	
	// No match was found
	px_free(host);
	px_free(ihost);
	return false;
	
	// A match was found
	match:
		px_free(host);
		px_free(ihost);
		return true;
}

/**
 * Creates a new pxProxyFactory instance.
 * 
 * @return A new pxProxyFactory instance or NULL on error
 */
pxProxyFactory *
px_proxy_factory_new ()
{
	pxProxyFactory *self = px_malloc0(sizeof(pxProxyFactory));
	unsigned int i;
	
	// Create the mutex
	pthread_mutex_init(&self->mutex, NULL);
	
	// Open the plugin dir
	DIR *plugindir = opendir(PLUGINDIR);
	if (!plugindir) return self;
	
	// Count the number of plugins
	for (i=0 ; readdir(plugindir) ; i++);
	self->plugins = (void **) px_malloc0(sizeof(void *) * (i + 1));
	rewinddir(plugindir);
	
	// For each plugin...
	struct dirent *ent;
	for (i=0 ; (ent = readdir(plugindir)) ; i++)
	{
		// Load the plugin
		char *tmp = px_strcat(PLUGINDIR, "/", ent->d_name, NULL);
		self->plugins[i] = dlopen(tmp, RTLD_LOCAL);
		px_free(tmp);
		if (!(self->plugins[i]))
		{
			i--;
			continue;
		}
		
		// Call the instantiation hook
		pxProxyFactoryBoolCallback instantiate;
		instantiate = dlsym(self->plugins[i], "on_proxy_factory_instantiate");
		if (instantiate && !instantiate(self))
		{
			dlclose(self->plugins[i]);
			self->plugins[i--] = NULL;
			continue;
		}
	}
	closedir(plugindir);
	
	return self;
}

bool
px_proxy_factory_config_add(pxProxyFactory *self, const char *name, pxConfigCategory category, pxProxyFactoryPtrCallback callback)
{
	int count;
	pxProxyFactoryConfig **tmp;
	
	// Verify some basic stuff
	if (!self)                      return false;
	if (!callback)                  return false;
	if (!name || !strcmp(name, "")) return false;
	
	// Allocate an empty config array if there is none
	if (!self->configs) self->configs = px_malloc0(sizeof(pxProxyFactoryConfig *));
	
	// Make sure that 'name' is unique
	// Also, get a count of how many configs we have
	for (count=0 ; self->configs[count] ; count++)
		if (!strcmp(self->configs[count]->name, name))
			return false;
	
	// Allocate new array, copy old values into it and free old array
	tmp = px_malloc0(sizeof(pxProxyFactoryConfig *) * (count + 2));
	memcpy(tmp, self->configs, sizeof(pxProxyFactoryConfig *) * count);
	px_free(self->configs);
	self->configs = tmp;
	
	// Add the new callback to the end
	self->configs[count]           = px_malloc0(sizeof(pxProxyFactoryConfig));
	self->configs[count]->category = category;
	self->configs[count]->name     = px_strdup(name);
	self->configs[count]->callback = callback;
	
	return true;
}

bool
px_proxy_factory_config_del(pxProxyFactory *self, const char *name)
{
	int i,j;
	
	// Verify some basic stuff
	if (!self)                      return false;
	if (!name || !strcmp(name, "")) return false;
	if (!self->configs)             return false;
	
	// Remove and shift all configs down (if found)
	for (i=0,j=0 ; self->configs[j]; i++,j++)
	{
		if (i != j)
			self->configs[j] = self->configs[i];
		else if (!strcmp(self->configs[i]->name, name))
		{
			px_free(self->configs[i]->name);
			px_free(self->configs[j--]);
		}
	}
	
	// If we have an empty array, free it
	if (!self->configs[0])
	{
		px_free(self->configs);
		self->configs = NULL;
	}
	
	return i != j ? true : false;
}

bool
px_proxy_factory_misc_set(pxProxyFactory *self, const char *key, const void *value)
{
	int count;
	pxKeyVal **tmp;
	
	// Verify some basic stuff
	if (!self)                                return false;
	if (!key || !strcmp(key, ""))             return false;
	
	// Allocate an empty config array if there is none
	if (!self->misc) self->misc = px_malloc0(sizeof(pxKeyVal *));
	
	// Count the number of values
	for (count=0 ; self->misc[count] ; count++);
	
	// Unset value
	if (!value)
	{
		// Remove the keyval, shifting downward
		for (int i=0,j=0 ; self->misc[i] ; i++, j++)
		{
			// If the key is found, remove it
			if (!strcmp(key, self->misc[i]->key))
			{
				px_free(self->misc[i]->key);
				px_free(self->misc[i]);
				self->misc[i] = NULL;
				count--;
				j--;
			}
			
			// Shift down
			if (i > 0 && j > 0)
				self->misc[j] = self->misc[i];
		}
		
		// Resize array
		tmp = px_malloc0(sizeof(pxKeyVal *) * (count + 1));
		memcpy(tmp, self->misc, sizeof(pxKeyVal *) * count);
		px_free(self->misc);
		self->misc = tmp;
		return true;
	}
	
	// Attempt to update the value within the array
	for (int i=0 ; self->misc[i] ; i++)
	{
		if (!strcmp(key, self->misc[i]->key))
		{
			self->misc[i]->value = (void *) value;
			return true;
		}
	}
	
	// The key was not found in the array, so add it
	tmp = px_malloc0(sizeof(pxKeyVal *) * (count + 2));
	memcpy(tmp, self->misc, sizeof(pxKeyVal *) * count);
	tmp[count]        = px_malloc0(sizeof(pxKeyVal));
	tmp[count]->key   = px_strdup(key);
	tmp[count]->value = (void *) value;
	px_free(self->misc);
	self->misc = tmp;
	return true;
}

void *
px_proxy_factory_misc_get(pxProxyFactory *self, const char *key)
{
	// Verify some basic stuff
	if (!self)                    return NULL;
	if (!key || !strcmp(key, "")) return NULL;
	if (!self->misc)              return NULL;
	
	// Find the value listed
	for (int i=0 ; self->misc[i] ; i++)
		if (!strcmp(key, self->misc[i]->key))
			return self->misc[i]->value;
	
	return NULL;
}

/**
 * Get which proxies to use for the specified URL.
 * 
 * A NULL-terminated array of proxy strings is returned.
 * If the first proxy fails, the second should be tried, etc...
 * 
 * The format of the returned proxy strings are as follows:
 *   - http://proxy:port
 *   - socks://proxy:port
 *   - direct://
 * @url The URL we are trying to reach
 * @return A NULL-terminated array of proxy strings to use
 */
char **
px_proxy_factory_get_proxies (pxProxyFactory *self, char *url)
{
	pxURL    *realurl  = px_url_new(url);
	pxConfig *config   = NULL;
	char    **response = px_strsplit("direct://", ";");
	char     *tmp = NULL, *order = NULL, **orderv = NULL;
	
	// Verify some basic stuff
	if (!self)                    goto do_return;
	if (!url || !strcmp(url, "")) goto do_return;
	if (!realurl)                 goto do_return;
	
	// Lock mutex
	pthread_mutex_lock(&self->mutex);
	
	// Call the events
	for (int i=0 ; self->on_get_proxy && self->on_get_proxy[i] ; i++)
		self->on_get_proxy[i](self);
	
	// If our config file is stale, close it
	if (self->cf && px_config_file_is_stale(self->cf))
	{
		px_config_file_free(self->cf);
		self->cf = NULL;
	}
	
	// Try to open our config file if we don't have one
	if (!self->cf)
		self->cf = px_config_file_new(SYSCONFDIR "/proxy.conf");

	// If we have a config file, load the order from it		
	if (self->cf)
		tmp = px_config_file_get_value(self->cf, PX_CONFIG_FILE_DEFAULT_SECTION, "config_order");
		
	// Attempt to get info from the environment
	order = getenv("PX_CONFIG_ORDER");
	
	// Create the config order
	order = px_strcat(tmp ? tmp : "", ",", order ? order : "", ",", DEFAULT_CONFIG_ORDER, NULL);
	px_free(tmp); tmp = NULL;
	
	// Create the config plugin order vector
	orderv = px_strsplit(order, ",");
	px_free(order);
	
	// Get the config by searching the config order
	for (int i=0 ; orderv[i] && !config ; i++)
	{
		// Get the category (if applicable)
		pxConfigCategory category;
		if (!strcmp(orderv[i], "USER"))
			category = PX_CONFIG_CATEGORY_USER;
		else if (!strcmp(orderv[i], "SESSION"))
			category = PX_CONFIG_CATEGORY_SESSION;
		else if (!strcmp(orderv[i], "SYSTEM"))
			category = PX_CONFIG_CATEGORY_SYSTEM;
		else
			category = PX_CONFIG_CATEGORY_NONE;
		
		for (int j=0 ; self->configs && self->configs[j] && !config ; j++)
		{
			if (category != PX_CONFIG_CATEGORY_NONE && self->configs[j]->category == category)
				config = self->configs[j]->callback(self);
			else if (category == PX_CONFIG_CATEGORY_NONE && !strcmp(self->configs[j]->name, orderv[i]))
				config = self->configs[j]->callback(self);
		}
	}
	px_strfreev(orderv);
	
	// No config was found via search order, call all plugins
	for (int i=0 ; self->configs && self->configs[i] && !config ; i++)
		config = self->configs[i]->callback(self);
	
	// No plugin returned a valid config, fall back to 'wpad://'
	if (!config)
	{
		fprintf(stderr, "*** Unable to locate valid config! Falling back to auto-detection...\n");
		config         = px_malloc0(sizeof(pxConfig));
		config->url    = px_strdup("wpad://");
		config->ignore = px_strdup("");
	}
	
	// If the config plugin returned an invalid config type or malformed URL, fall back to 'wpad://'
	if (!(!strncmp(config->url, "http://", 7) || 
		  !strncmp(config->url, "socks://", 8) ||
		  !strncmp(config->url, "pac+", 4) ||
		  !strcmp (config->url, "wpad://") ||
		  !strcmp (config->url, "direct://")))
	{
		fprintf(stderr, "*** Config plugin returned invalid URL type! Falling back to auto-detection...\n");
		px_free(config->url);
		config->url = px_strdup("wpad://");
	}
	else if (!strncmp(config->url, "pac+", 4) && !px_url_is_valid(config->url + 4))
	{
		fprintf(stderr, "*** Config plugin returned malformed URL! Falling back to auto-detection...\n");
		px_free(config->url);
		config->url = px_strdup("wpad://");
	}
	else if ((!strncmp(config->url, "http://", 7) || !strncmp(config->url, "socks://", 8)) && 
			  !px_url_is_valid(config->url))
	{
		fprintf(stderr, "*** Config plugin returned malformed URL! Falling back to auto-detection...\n");
		px_free(config->url);
		config->url = px_strdup("wpad://");
	}
	
	// Check our ignore patterns
	char **ignores = px_strsplit(config->ignore, ",");
	for (int i=0 ; ignores[i] ; i++)
	{
		if (_domain_ignore(realurl, ignores[i]) || _ip_ignore(realurl, ignores[i]))
		{
			px_strfreev(ignores);
			goto do_return;
		}
	}
	px_strfreev(ignores);
	
	// If we have a wpad config
	if (!strcmp(config->url, "wpad://"))
	{
		// Get the WPAD object if needed
		if (!self->wpad)
		{
			if (self->pac) px_pac_free(self->pac);
			self->pac  = NULL;
			self->wpad = px_wpad_new();
			if (!self->wpad)
			{
				fprintf(stderr, "*** Unable to create WPAD! Falling back to direct...\n");
				goto do_return;
			}
		}
		
		// If we have no PAC, get one
		// If getting the PAC fails, but the WPAD cycle worked, restart the cycle
		if (!self->pac && !(self->pac = px_wpad_next(self->wpad)) && px_wpad_pac_found(self->wpad))
		{
			px_wpad_rewind(self->wpad);
			self->pac = px_wpad_next(self->wpad);
		}
		
		// If the WPAD cycle failed, fall back to direct
		if (!self->pac)
		{
			fprintf(stderr, "*** Unable to locate PAC! Falling back to direct...\n");
			goto do_return;
		}
		
		// Run the PAC
		if (self->pac_runner)
		{
			px_strfreev(response);
			response = _format_pac_response(self->pac_runner(self, self->pac, realurl));
		}
		
		// No PAC runner found, fall back to direct
		else
			fprintf(stderr, "*** PAC found, but no active PAC runner! Falling back to direct...\n");
	}
	
	// If we have a PAC config
	else if (!strncmp(config->url, "pac+", 4))
	{
		// Clear WPAD to indicate that this is a non-WPAD PAC
		if (self->wpad)
		{
			px_wpad_free(self->wpad);
			self->wpad = NULL;
		}
		
		// If a PAC alread exists, but comes from a different URL than the one
		// specified, remove it
		if (self->pac)
		{
			pxURL *urltmp = px_url_new(config->url + 4);
			if (!urltmp)
			{
				fprintf(stderr, "*** Invalid PAC URL! Falling back to direct...\n");
				goto do_return;
			}
			if (!px_url_equals(urltmp, px_pac_get_url(self->pac)))
			{
				px_pac_free(self->pac);
				self->pac = NULL;
			}
			px_url_free(urltmp);
		}
		
		// Try to load the PAC if it is not already loaded
		if (!self->pac && !(self->pac = px_pac_new_from_string(config->url + 4)))
		{
			fprintf(stderr, "*** Invalid PAC URL! Falling back to direct...\n");
			goto do_return;
		}

		// Run the PAC
		if (self->pac_runner)
		{
			px_strfreev(response);
			response = _format_pac_response(self->pac_runner(self, self->pac, realurl));
		}
		else
			fprintf(stderr, "*** PAC found, but no active PAC runner! Falling back to direct...\n");
	}
	
	// If we have a manual config (http://..., socks://...)
	else if (!strncmp(config->url, "http://", 7) || !strncmp(config->url, "socks://", 8))
	{
		if (self->wpad) { px_wpad_free(self->wpad); self->wpad = NULL; }
		if (self->pac)  { px_pac_free(self->pac);   self->pac = NULL; }
		px_strfreev(response);
		response = px_strsplit(config->url, ";");
	}
	
	// Actually return, freeing misc stuff
	do_return:
		if (config)  { px_free(config->url); px_free(config->ignore); px_free(config); }
		if (realurl) px_url_free(realurl);
		if (self)    pthread_mutex_unlock(&self->mutex);
		return response;
}

bool
px_proxy_factory_on_get_proxy_add (pxProxyFactory *self, pxProxyFactoryVoidCallback callback)
{
	int count;
	pxProxyFactoryVoidCallback *tmp;
	
	// Verify some basic stuff
	if (!self)     return false;
	if (!callback) return false;
	
	// Allocate an empty config array if there is none
	if (!self->on_get_proxy) self->on_get_proxy = px_malloc0(sizeof(pxProxyFactoryVoidCallback));
	
	// Get a count of how many callbacks we have
	for (count=0 ; self->on_get_proxy[count] ; count++);
	
	// Allocate new array, copy old values into it and free old array
	tmp = px_malloc0(sizeof(pxProxyFactoryVoidCallback) * (count + 2));
	memcpy(tmp, self->on_get_proxy, sizeof(pxProxyFactoryVoidCallback) * count);
	px_free(self->on_get_proxy);
	self->on_get_proxy = tmp;
	
	// Add the new callback to the end
	self->on_get_proxy[count] = callback;
	
	return true;
}

bool
px_proxy_factory_on_get_proxy_del (pxProxyFactory *self, pxProxyFactoryVoidCallback callback)
{
	int i,j;
	
	// Verify some basic stuff
	if (!self)               return false;
	if (!callback)           return false;
	if (!self->on_get_proxy) return false;
	
	// Remove and shift all callbacks down (if found)
	for (i=0,j=0 ; self->on_get_proxy[j]; i++,j++)
	{
		if (i != j)
			self->on_get_proxy[j] = self->on_get_proxy[i];
		else if (self->on_get_proxy[i] == callback)
			self->on_get_proxy[j--] = NULL;
	}
	
	// If we have an empty array, free it
	if (!self->on_get_proxy[0])
	{
		px_free(self->on_get_proxy);
		self->on_get_proxy = NULL;
	}
	
	return i != j ? true : false;
}

bool
px_proxy_factory_pac_runner_set (pxProxyFactory *self, pxPACRunnerCallback callback)
{
	if (!self) return false;
	if (self->pac_runner && callback && self->pac_runner != callback) return false;
	self->pac_runner = callback;
	return true;
}

void
px_proxy_factory_network_changed(pxProxyFactory *self)
{
	if (self->wpad)
	{
		px_wpad_free(self->wpad);
		self->wpad = NULL;
	}
	
	if (self->pac)
	{
		px_pac_free(self->pac);
		self->pac = NULL;
	}
}

/**
 * Frees the pxProxyFactory instance when no longer used.
 */
void
px_proxy_factory_free (pxProxyFactory *self)
{
	unsigned int i;
	
	if (!self) return;
	
	pthread_mutex_lock(&self->mutex);
	
	// Free the plugins
	if (self->plugins)
	{
		for (i=0 ; self->plugins[i] ; i++)
		{
			// Call the destantiation hook
			pxProxyFactoryVoidCallback destantiate;
			destantiate = dlsym(self->plugins[i], "on_proxy_factory_destantiate");
			if (destantiate)
				destantiate(self);
			
			// Unload the plugin
			dlclose(self->plugins[i]);
			self->plugins[i] = NULL;
		}
		px_free(self->plugins);
	}
	
	// Free misc
	if (self->misc)
	{
		for (i=0 ; self->misc[i] ; i++)
		{
			px_free(self->misc[i]->key);
			px_free(self->misc[i]);
		}
		px_free(self->misc);
	}
	
	// Free everything else
	px_pac_free(self->pac);
	px_wpad_free(self->wpad);
	px_config_file_free(self->cf);
	pthread_mutex_unlock(&self->mutex);
	pthread_mutex_destroy(&self->mutex);
	px_free(self);
}

/**
 * Utility function to create pxConfig objects. Steals ownership of the parameters.
 * @url The proxy config url.  If NULL, no pxConfig will be created.
 * @ignore Ignore patterns.  If NULL, a pxConfig will still be created.
 * @return pxConfig instance or NULL if url is NULL.
 */
pxConfig *
px_config_create(char *url, char *ignore)
{
	if (!url) return NULL;
	pxConfig *config = px_malloc0(sizeof(pxConfig));
	config->url      = url;
	config->ignore   = ignore ? ignore : px_strdup("");
	return config;
}
