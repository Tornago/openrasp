/*
 * Copyright 2017-2019 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils/JsonReader.h"
#include "utils/YamlReader.h"
#include "utils/file.h"
#include "openrasp.h"
#include "openrasp_ini.h"

extern "C"
{
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
}
#include "openrasp_log.h"
#include "openrasp_v8.h"
#include "openrasp_hook.h"
#include "openrasp_inject.h"
#include "openrasp_security_policy.h"
#include "openrasp_output_detect.h"
#include "openrasp_check_type.h"
#ifdef HAVE_FSWATCH
#include "openrasp_fswatch.h"
#endif
#include <new>
#include "agent/shared_config_manager.h"
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
#include "agent/openrasp_agent_manager.h"
#endif

using openrasp::ConfigHolder;

ZEND_DECLARE_MODULE_GLOBALS(openrasp);

bool is_initialized = false;
bool remote_active = false;
static std::string get_config_abs_path(ConfigHolder::FromType type);
static bool update_config(openrasp::ConfigHolder *config, ConfigHolder::FromType type = ConfigHolder::FromType::kYaml);
static void hook_without_params(OpenRASPCheckType check_type);

PHP_INI_BEGIN()
PHP_INI_ENTRY1("openrasp.root_dir", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.root_dir)
#ifdef HAVE_GETTEXT
PHP_INI_ENTRY1("openrasp.locale", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.locale)
#endif
PHP_INI_ENTRY1("openrasp.backend_url", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.backend_url)
PHP_INI_ENTRY1("openrasp.app_id", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.app_id)
PHP_INI_ENTRY1("openrasp.app_secret", "", PHP_INI_SYSTEM, OnUpdateOpenraspCString, &openrasp_ini.app_secret)
PHP_INI_ENTRY1("openrasp.remote_management_enable", "off", PHP_INI_SYSTEM, OnUpdateOpenraspBool, &openrasp_ini.remote_management_enable)
PHP_INI_ENTRY1("openrasp.heartbeat_interval", "180", PHP_INI_SYSTEM, OnUpdateOpenraspHeartbeatInterval, &openrasp_ini.heartbeat_interval)
PHP_INI_END()

PHP_GINIT_FUNCTION(openrasp)
{
#if defined(COMPILE_DL_JSON) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
#ifdef ZTS
    new (openrasp_globals) _zend_openrasp_globals;
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (!openrasp::oam)
    {
        update_config(&(openrasp_globals->config));
    }
#else
    update_config(&(openrasp_globals->config));
#endif
#endif
}

PHP_GSHUTDOWN_FUNCTION(openrasp)
{
#ifdef ZTS
    openrasp_globals->~_zend_openrasp_globals();
#endif
}

PHP_MINIT_FUNCTION(openrasp)
{
    ZEND_INIT_MODULE_GLOBALS(openrasp, PHP_GINIT(openrasp), PHP_GSHUTDOWN(openrasp));
    REGISTER_INI_ENTRIES();
    if (!current_sapi_supported())
    {
        return SUCCESS;
    }
    if (!make_openrasp_root_dir(openrasp_ini.root_dir))
    {
        return SUCCESS;
    }
    std::string locale_path(std::string(openrasp_ini.root_dir) + DEFAULT_SLASH + "locale" + DEFAULT_SLASH);
    openrasp_set_locale(openrasp_ini.locale, locale_path.c_str());
    openrasp::scm.reset(new openrasp::SharedConfigManager());
    if (!openrasp::scm->startup())
    {
        openrasp_error(LEVEL_WARNING, RUNTIME_ERROR, _("Fail to startup SharedConfigManager."));
        return SUCCESS;
    }

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (need_alloc_shm_current_sapi() && openrasp_ini.remote_management_enable)
    {
        openrasp::oam.reset(new openrasp::OpenraspAgentManager());
        if (!openrasp::oam->verify_ini_correct())
        {
            return SUCCESS;
        }
        remote_active = true;
    }
#endif
    if (!remote_active)
    {
        update_config(&OPENRASP_G(config));
    }

    if (PHP_MINIT(openrasp_log)(INIT_FUNC_ARGS_PASSTHRU) == FAILURE)
    {
        return SUCCESS;
    }
    if (PHP_MINIT(openrasp_v8)(INIT_FUNC_ARGS_PASSTHRU) == FAILURE)
    {
        return SUCCESS;
    }
    int result;
    result = PHP_MINIT(openrasp_hook)(INIT_FUNC_ARGS_PASSTHRU);
    result = PHP_MINIT(openrasp_inject)(INIT_FUNC_ARGS_PASSTHRU);

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (remote_active && openrasp::oam)
    {
        openrasp::oam->startup();
    }
#endif
    if (!remote_active)
    {
        std::string config_file_path = get_config_abs_path(ConfigHolder::FromType::kYaml);
        std::string conf_contents;
        if (openrasp::read_entire_content(config_file_path, conf_contents))
        {
            openrasp::YamlReader yreader(conf_contents);
            std::vector<std::string> hook_white_key({"hook.white"});
            std::map<std::string, std::vector<std::string>> hook_white_map;
            std::vector<std::string> url_keys = yreader.fetch_object_keys(hook_white_key);
            for (auto &key_item : url_keys)
            {
                hook_white_key.push_back(key_item);
                std::vector<std::string> white_types = yreader.fetch_strings(hook_white_key, {});
                hook_white_key.pop_back();
                hook_white_map.insert({key_item, white_types});
            }
            openrasp::scm->build_check_type_white_array(hook_white_map);
        }
#ifdef HAVE_FSWATCH
        result = PHP_MINIT(openrasp_fswatch)(INIT_FUNC_ARGS_PASSTHRU);
#endif
    }

    result = PHP_MINIT(openrasp_security_policy)(INIT_FUNC_ARGS_PASSTHRU);
    result = PHP_MINIT(openrasp_output_detect)(INIT_FUNC_ARGS_PASSTHRU);
    is_initialized = true;
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        if (!remote_active)
        {
#ifdef HAVE_FSWATCH
            result = PHP_MSHUTDOWN(openrasp_fswatch)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
#endif
        }
        result = PHP_MSHUTDOWN(openrasp_inject)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_hook)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_v8)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_MSHUTDOWN(openrasp_log)(SHUTDOWN_FUNC_ARGS_PASSTHRU);

#ifdef HAVE_OPENRASP_REMOTE_MANAGER
        if (remote_active && openrasp::oam)
        {
            openrasp::oam->shutdown();
        }
        openrasp::oam.reset();
#endif
        openrasp::scm->shutdown();
        openrasp::scm.reset();
        remote_active = false;
        is_initialized = false;
    }
    UNREGISTER_INI_ENTRIES();
    ZEND_SHUTDOWN_MODULE_GLOBALS(openrasp, PHP_GSHUTDOWN(openrasp));
    return SUCCESS;
}

PHP_RINIT_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        long config_last_update = openrasp::scm->get_config_last_update();
        if (config_last_update && config_last_update > OPENRASP_G(config).GetLatestUpdateTime())
        {
            if (update_config(&OPENRASP_G(config), ConfigHolder::FromType::kJson))
            {
                OPENRASP_G(config).SetLatestUpdateTime(config_last_update);
            }
        }
        // openrasp_inject must be called before openrasp_log cuz of request_id
        result = PHP_RINIT(openrasp_inject)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_log)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_hook)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_v8)(INIT_FUNC_ARGS_PASSTHRU);
        result = PHP_RINIT(openrasp_output_detect)(INIT_FUNC_ARGS_PASSTHRU);
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
        if (remote_active && openrasp::oam)
        {
            zval *http_global_server = fetch_http_globals(TRACK_VARS_SERVER);
            if (http_global_server)
            {
                const char *webroot = fetch_outmost_string_from_ht(Z_ARRVAL_P(http_global_server), "DOCUMENT_ROOT");
                if (webroot && openrasp::oam->path_writable() && !openrasp::oam->path_exist(zend_inline_hash_func(webroot, strlen(webroot))))
                {
                    openrasp::oam->write_webroot_path(webroot);
                }
            }
        }
#endif
        hook_without_params(REQUEST);
    }
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(openrasp)
{
    if (is_initialized)
    {
        int result;
        hook_without_params(REQUEST_END);
        result = PHP_RSHUTDOWN(openrasp_log)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
        result = PHP_RSHUTDOWN(openrasp_inject)(SHUTDOWN_FUNC_ARGS_PASSTHRU);
    }
    return SUCCESS;
}

PHP_MINFO_FUNCTION(openrasp)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Status", is_initialized ? "Protected" : "Unprotected, Initialization Failed");
    php_info_print_table_row(2, "Version", PHP_OPENRASP_VERSION);
#ifdef OPENRASP_BUILD_TIME
    php_info_print_table_row(2, "Build Time", OPENRASP_BUILD_TIME);
#endif
#ifdef OPENRASP_COMMIT_ID
    php_info_print_table_row(2, "Commit Id", OPENRASP_COMMIT_ID);
#else
    php_info_print_table_row(2, "Commit Id", "");
#endif
    php_info_print_table_row(2, "V8 Version", ZEND_TOSTR(V8_MAJOR_VERSION) "." ZEND_TOSTR(V8_MINOR_VERSION));
    php_info_print_table_row(2, "Antlr Version", "4.7.1 (JavaScript Runtime)");
#ifdef HAVE_OPENRASP_REMOTE_MANAGER
    if (remote_active && openrasp::oam)
    {
        const char *plugin_version = openrasp::oam->get_plugin_version();
        php_info_print_table_row(2, "Plugin Version", plugin_version ? plugin_version : "");
    }
#endif
    php_info_print_table_end();
    DISPLAY_INI_ENTRIES();
}

/**  module depends
 */
#if ZEND_MODULE_API_NO >= 20050922
zend_module_dep openrasp_deps[] = {
    ZEND_MOD_REQUIRED("standard")
        ZEND_MOD_REQUIRED("json")
            ZEND_MOD_CONFLICTS("xdebug")
                ZEND_MOD_END};
#endif

zend_module_entry openrasp_module_entry = {
#if ZEND_MODULE_API_NO >= 20050922
    STANDARD_MODULE_HEADER_EX,
    NULL,
    openrasp_deps,
#else
    STANDARD_MODULE_HEADER,
#endif
    "openrasp",
    NULL,
    PHP_MINIT(openrasp),
    PHP_MSHUTDOWN(openrasp),
    PHP_RINIT(openrasp),
    PHP_RSHUTDOWN(openrasp),
    PHP_MINFO(openrasp),
    PHP_OPENRASP_VERSION,
    STANDARD_MODULE_PROPERTIES};

#ifdef COMPILE_DL_OPENRASP
ZEND_GET_MODULE(openrasp)
#endif

static std::string get_config_abs_path(ConfigHolder::FromType type)
{
    std::string filename;
    switch (type)
    {
    case ConfigHolder::FromType::kJson:
        filename = "cloud-config.json";
        break;
    case ConfigHolder::FromType::kYaml:
    default:
        filename = "openrasp.yml";
        break;
    }
    return std::string(openrasp_ini.root_dir) +
           DEFAULT_SLASH +
           "conf" +
           DEFAULT_SLASH + filename;
}

static bool update_config(openrasp::ConfigHolder *config, ConfigHolder::FromType type)
{
    if (nullptr != openrasp_ini.root_dir && strcmp(openrasp_ini.root_dir, "") != 0)
    {
        std::string config_file_path = get_config_abs_path(type);
        std::string conf_contents;
        if (openrasp::read_entire_content(config_file_path, conf_contents))
        {
            std::shared_ptr<openrasp::BaseReader> config_reader = nullptr;
            switch (type)
            {
            case ConfigHolder::FromType::kJson:
                config_reader.reset(new openrasp::JsonReader());
                break;
            case ConfigHolder::FromType::kYaml:
            default:
                config_reader.reset(new openrasp::YamlReader());
                break;
            }
            if (config_reader)
            {
                config_reader->load(conf_contents);
                if (config_reader->has_error())
                {
                    openrasp_error(LEVEL_WARNING, CONFIG_ERROR, _("Fail to parse config, cuz of %s."),
                                   config_reader->get_error_msg().c_str());
                }
                else
                {
                    return config->update(config_reader.get());
                }
            }
        }
    }
    return false;
}

static void hook_without_params(OpenRASPCheckType check_type)
{
    bool type_ignored = openrasp_check_type_ignored(check_type);
    if (type_ignored)
    {
        return;
    }
    openrasp::Isolate *isolate = OPENRASP_V8_G(isolate);
    if (!isolate)
    {
        return;
    }
    bool is_block = false;
    {
        v8::HandleScope handle_scope(isolate);
        auto params = v8::Object::New(isolate);
        is_block = isolate->Check(openrasp::NewV8String(isolate, get_check_type_name(check_type)), params,
                                  OPENRASP_CONFIG(plugin.timeout.millis));
    }
    if (is_block)
    {
        handle_block();
    }
}