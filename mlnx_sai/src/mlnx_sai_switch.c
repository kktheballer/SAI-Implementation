/*
 *  Copyright (C) 2017. Mellanox Technologies, Ltd. ALL RIGHTS RESERVED.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License"); you may
 *    not use this file except in compliance with the License. You may obtain
 *    a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 *    THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR
 *    CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 *    LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 *    FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 *    See the Apache Version 2.0 License for specific language governing
 *    permissions and limitations under the License.
 *
 */

#include "sai_windows.h"
#include "sai.h"
#include "mlnx_sai.h"
#include "syslog.h"
#include <errno.h>
#include "assert.h"
#ifndef _WIN32
#include <netinet/ether.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <sys/mman.h>
#include <pthread.h>
#endif
#include <complib/cl_mem.h>
#include <complib/cl_passivelock.h>
#include <complib/cl_shared_memory.h>
#include <complib/cl_thread.h>
#include <math.h>
#include <limits.h>
#include <sx/sdk/sx_api_rm.h>
#include "meta/saimetadata.h"

#ifdef _WIN32
#undef CONFIG_SYSLOG
#endif

#undef  __MODULE__
#define __MODULE__ SAI_SWITCH

#define SDK_START_CMD_STR_LEN 255

#define SAI_KEY_IPV4_ROUTE_TABLE_SIZE    "SAI_IPV4_ROUTE_TABLE_SIZE"
#define SAI_KEY_IPV6_ROUTE_TABLE_SIZE    "SAI_IPV6_ROUTE_TABLE_SIZE"
#define SAI_KEY_IPV4_NEIGHBOR_TABLE_SIZE "SAI_IPV4_NEIGHBOR_TABLE_SIZE"
#define SAI_KEY_IPV6_NEIGHBOR_TABLE_SIZE "SAI_IPV6_NEIGHBOR_TABLE_SIZE"

#define MAX_BFD_SESSION_NUMBER 64

typedef struct _sai_switch_notification_t {
    sai_switch_state_change_notification_fn     on_switch_state_change;
    sai_fdb_event_notification_fn               on_fdb_event;
    sai_port_state_change_notification_fn       on_port_state_change;
    sai_switch_shutdown_request_notification_fn on_switch_shutdown_request;
    sai_packet_event_notification_fn            on_packet_event;
    sai_bfd_session_state_change_notification_fn on_bfd_session_state_change;
} sai_switch_notification_t;

static sx_verbosity_level_t LOG_VAR_NAME(__MODULE__) = SX_VERBOSITY_LEVEL_WARNING;
sx_api_handle_t                  gh_sdk = 0;
static sai_switch_notification_t g_notification_callbacks;
static sai_switch_profile_id_t   g_profile_id;
rm_resources_t                   g_resource_limits;
sai_db_t                        *g_sai_db_ptr              = NULL;
sai_qos_db_t                    *g_sai_qos_db_ptr          = NULL;
uint32_t                         g_sai_qos_db_size         = 0;
sai_buffer_db_t                 *g_sai_buffer_db_ptr       = NULL;
uint32_t                         g_sai_buffer_db_size      = 0;
mlnx_acl_db_t                   *g_sai_acl_db_ptr          = NULL;
uint32_t                         g_sai_acl_db_size         = 0;
uint32_t                         g_sai_acl_db_pbs_map_size = 0;
sai_tunnel_db_t                 *g_sai_tunnel_db_ptr       = NULL;
uint32_t                         g_sai_tunnel_db_size      = 0;
static cl_thread_t               event_thread;
static bool                      event_thread_asked_to_stop = false;
static uint32_t                  g_fdb_table_size;
static uint32_t                  g_route_table_size, g_ipv4_route_table_size, g_ipv6_route_table_size;
static uint32_t                  g_neighbor_table_size, g_ipv4_neighbor_table_size, g_ipv6_neighbor_table_size;
static bool                      g_uninit_data_plane_on_removal = true;
static uint32_t                  g_mlnx_shm_rm_size             = 0;

void log_cb(sx_log_severity_t severity, const char *module_name, char *msg);
void log_pause_cb(void);
#ifdef CONFIG_SYSLOG
sx_log_cb_t sai_log_cb = log_cb;
static bool g_log_init = false;
#else
sx_log_cb_t sai_log_cb = NULL;
#endif

#define MLNX_SHM_RM_ARRAY_CANARY_BASE ((mlnx_shm_array_canary_t)(0x0EC0))
#define MLNX_SHM_RM_ARRAY_CANARY(type) \
    ((mlnx_shm_array_canary_t)MLNX_SHM_RM_ARRAY_CANARY_BASE + \
     (type))
#define MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type) \
    ((MLNX_SHM_RM_ARRAY_TYPE_MIN <= (type)) && \
     ((type) <= MLNX_SHM_RM_ARRAY_TYPE_MAX))
#define MLNX_SHM_RM_ARRAY_HDR_IS_VALID(type, array_hdr) ((array_hdr)->canary == MLNX_SHM_RM_ARRAY_CANARY(type))
#define MLNX_SHM_RM_ARRAY_BASE_PTR ((uint8_t*)g_sai_db_ptr->array_info + sizeof(g_sai_db_ptr->array_info))

typedef struct _sx_pool_info {
    uint32_t           pool_id;
    sx_cos_pool_attr_t pool_attr;
} sx_pool_info_t;
typedef struct _pool_array_info_t {
    sx_pool_info_t* pool_arr;
    uint32_t        pool_cnt;
} pool_array_info_t;

sai_status_t mlnx_hash_ecmp_sx_config_update(void);
sai_status_t mlnx_hash_lag_sx_config_update(void);
sai_status_t mlnx_hash_object_is_applicable(_In_ sai_object_id_t                    hash_oid,
                                            _In_ mlnx_switch_usage_hash_object_id_t hash_oper_id,
                                            _Out_ bool                             *is_aplicable);
sai_status_t mlnx_hash_config_db_changes_commit(_In_ mlnx_switch_usage_hash_object_id_t hash_oper_id);
sai_status_t mlnx_sai_log_levels_post_init(void);
sai_status_t mlnx_debug_counter_switch_stats_get(_In_ uint32_t             number_of_counters,
                                                 _In_ const sai_stat_id_t *counter_ids,
                                                 _In_ bool                 read,
                                                 _In_ bool                 clear,
                                                 _Out_ uint64_t           *counters);
static sai_status_t mlnx_sai_rm_db_init(void);
static sai_status_t switch_open_traps(void);
static sai_status_t switch_close_traps(void);
static void event_thread_func(void *context);
static sai_status_t sai_db_create();
static void sai_db_values_init();
static sai_status_t mlnx_parse_config(const char *config_file);
static uint32_t sai_qos_db_size_get();
static void sai_qos_db_init();
static sai_status_t sai_qos_db_unload(boolean_t erase_db);
static sai_status_t sai_qos_db_create();
static void sai_buffer_db_values_init();
static sai_status_t sai_buffer_db_switch_connect_init(int shmid);
static void sai_buffer_db_data_reset();
static uint32_t sai_buffer_db_size_get();
static void sai_buffer_db_pointers_init();
static sai_status_t sai_buffer_db_unload(boolean_t erase_db);
static sai_status_t sai_buffer_db_create();
static bool is_prime_number(uint32_t a);
static uint32_t sai_acl_db_pbs_map_size_get();
static uint32_t sai_acl_db_size_get();
static sai_status_t sai_acl_db_create();
static void sai_acl_db_init();
static sai_status_t sai_acl_db_switch_connect_init(int shmid);
static sai_status_t sai_acl_db_unload(boolean_t erase_db);
static uint32_t sai_udf_db_size_get();
static void sai_udf_db_init();
static sai_status_t sai_tunnel_db_unload(boolean_t erase_db);
static sai_status_t sai_tunnel_db_create();
static uint32_t sai_tunnel_db_size_get();
static void sai_tunnel_db_init();
static sai_status_t sai_tunnel_db_switch_connect_init(int shmid);
static sai_status_t mlnx_switch_fdb_record_age(_In_ const sx_fdb_notify_record_t *fdb_record);
static sai_status_t mlnx_switch_fdb_record_check_and_age(_In_ const sx_fdb_notify_record_t *fdb_record);
static sai_status_t mlnx_switch_restart_warm();
static sai_status_t mlnx_switch_warm_recover(_In_ sai_object_id_t switch_id);
static sai_status_t mlnx_switch_vxlan_udp_port_set(uint16_t udp_port);
static sai_status_t mlnx_switch_crc_params_apply(bool init);
static sai_status_t mlnx_switch_port_number_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg);
static sai_status_t mlnx_switch_max_ports_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_switch_port_list_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_switch_cpu_port_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg);
static sai_status_t mlnx_switch_max_mtu_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg);
static sai_status_t mlnx_switch_max_vr_get(_In_ const sai_object_key_t   *key,
                                           _Inout_ sai_attribute_value_t *value,
                                           _In_ uint32_t                  attr_index,
                                           _Inout_ vendor_cache_t        *cache,
                                           void                          *arg);
static sai_status_t mlnx_switch_fdb_size_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg);
static sai_status_t mlnx_switch_neighbor_size_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg);
static sai_status_t mlnx_switch_route_size_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_on_link_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg);
static sai_status_t mlnx_switch_oper_status_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg);
static sai_status_t mlnx_switch_max_temp_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg);
static sai_status_t mlnx_switch_acl_table_min_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_acl_table_max_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_acl_entry_min_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_acl_entry_max_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_acl_table_group_min_prio_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg);
static sai_status_t mlnx_switch_acl_table_group_max_prio_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg);
static sai_status_t mlnx_switch_acl_capability_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg);
static sai_status_t mlnx_switch_max_acl_action_count_get(_In_ const sai_object_key_t   *key,
                                                         _Inout_ sai_attribute_value_t *value,
                                                         _In_ uint32_t                  attr_index,
                                                         _Inout_ vendor_cache_t        *cache,
                                                         void                          *arg);
static sai_status_t mlnx_switch_max_acl_range_count_get(_In_ const sai_object_key_t   *key,
                                                        _Inout_ sai_attribute_value_t *value,
                                                        _In_ uint32_t                  attr_index,
                                                        _Inout_ vendor_cache_t        *cache,
                                                        void                          *arg);
static sai_status_t mlnx_switch_acl_trap_range_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg);
static sai_status_t mlnx_switch_acl_meta_range_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg);
static sai_status_t mlnx_switch_max_lag_members_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg);
static sai_status_t mlnx_switch_max_lag_number_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg);
static sai_status_t mlnx_switch_mode_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg);
static sai_status_t mlnx_switch_src_mac_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg);
static sai_status_t mlnx_switch_aging_time_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_fdb_flood_ctrl_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg);
static sai_status_t mlnx_switch_ecmp_hash_param_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg);
static sai_status_t mlnx_switch_ecmp_members_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg);
static sai_status_t mlnx_switch_ecmp_groups_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg);
static sai_status_t mlnx_switch_counter_refresh_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg);
static sai_status_t mlnx_switch_default_trap_group_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_default_vrid_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg);
static sai_status_t mlnx_switch_sched_group_levels_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_sched_groups_count_per_level_get(_In_ const sai_object_key_t   *key,
                                                                 _Inout_ sai_attribute_value_t *value,
                                                                 _In_ uint32_t                  attr_index,
                                                                 _Inout_ vendor_cache_t        *cache,
                                                                 void                          *arg);
static sai_status_t mlnx_switch_sched_max_child_groups_count_get(_In_ const sai_object_key_t   *key,
                                                                 _Inout_ sai_attribute_value_t *value,
                                                                 _In_ uint32_t                  attr_index,
                                                                 _Inout_ vendor_cache_t        *cache,
                                                                 void                          *arg);
static sai_status_t mlnx_switch_queue_num_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_switch_lag_hash_attr_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg);
static sai_status_t mlnx_switch_init_connect_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg);
static sai_status_t mlnx_switch_profile_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_event_func_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_mode_set(_In_ const sai_object_key_t      *key,
                                         _In_ const sai_attribute_value_t *value,
                                         void                             *arg);
static sai_status_t mlnx_switch_aging_time_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg);
static sai_status_t mlnx_switch_fdb_flood_ctrl_set(_In_ const sai_object_key_t      *key,
                                                   _In_ const sai_attribute_value_t *value,
                                                   void                             *arg);
static sai_status_t mlnx_switch_ecmp_hash_param_set(_In_ const sai_object_key_t      *key,
                                                    _In_ const sai_attribute_value_t *value,
                                                    void                             *arg);
static sai_status_t mlnx_switch_counter_refresh_set(_In_ const sai_object_key_t      *key,
                                                    _In_ const sai_attribute_value_t *value,
                                                    void                             *arg);
static sai_status_t mlnx_switch_lag_hash_attr_set(_In_ const sai_object_key_t      *key,
                                                  _In_ const sai_attribute_value_t *value,
                                                  void                             *arg);
static sai_status_t mlnx_switch_qos_map_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_qos_map_id_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg);
static sai_status_t mlnx_switch_default_tc_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_default_tc_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg);
static sai_status_t mlnx_switch_hash_object_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg);
static sai_status_t mlnx_switch_hash_object_set(_In_ const sai_object_key_t      *key,
                                                _In_ const sai_attribute_value_t *value,
                                                void                             *arg);
static sai_status_t mlnx_switch_restart_warm_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg);
static sai_status_t mlnx_switch_restart_warm_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg);
static sai_status_t mlnx_switch_warm_recover_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg);
static sai_status_t mlnx_switch_warm_recover_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg);
static sai_status_t mlnx_switch_total_pool_buffer_size_get(_In_ const sai_object_key_t   *key,
                                                           _Inout_ sai_attribute_value_t *value,
                                                           _In_ uint32_t                  attr_index,
                                                           _Inout_ vendor_cache_t        *cache,
                                                           void                          *arg);
static sai_status_t mlnx_switch_ingress_pool_num_get(_In_ const sai_object_key_t   *key,
                                                     _Inout_ sai_attribute_value_t *value,
                                                     _In_ uint32_t                  attr_index,
                                                     _Inout_ vendor_cache_t        *cache,
                                                     void                          *arg);
static sai_status_t mlnx_switch_egress_pool_num_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg);
static sai_status_t mlnx_default_vlan_id_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg);
static sai_status_t mlnx_default_stp_id_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg);
static sai_status_t mlnx_max_stp_instance_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_qos_max_tcs_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg);
static sai_status_t mlnx_restart_type_get(_In_ const sai_object_key_t   *key,
                                          _Inout_ sai_attribute_value_t *value,
                                          _In_ uint32_t                  attr_index,
                                          _Inout_ vendor_cache_t        *cache,
                                          void                          *arg);
static sai_status_t mlnx_min_restart_interval_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg);
static sai_status_t mlnx_nv_storage_get(_In_ const sai_object_key_t   *key,
                                        _Inout_ sai_attribute_value_t *value,
                                        _In_ uint32_t                  attr_index,
                                        _Inout_ vendor_cache_t        *cache,
                                        void                          *arg);
static sai_status_t mlnx_switch_event_func_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg);
static sai_status_t mlnx_switch_transaction_mode_set(_In_ const sai_object_key_t      *key,
                                                     _In_ const sai_attribute_value_t *value,
                                                     void                             *arg);
static sai_status_t mlnx_switch_transaction_mode_get(_In_ const sai_object_key_t   *key,
                                                     _Inout_ sai_attribute_value_t *value,
                                                     _In_ uint32_t                  attr_index,
                                                     _Inout_ vendor_cache_t        *cache,
                                                     void                          *arg);
static sai_status_t mlnx_default_bridge_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg);
static sai_status_t mlnx_switch_available_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_switch_acl_available_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg);
static sai_status_t mlnx_switch_vxlan_default_port_set(_In_ const sai_object_key_t      *key,
                                                       _In_ const sai_attribute_value_t *value,
                                                       void                             *arg);
static sai_status_t mlnx_switch_vxlan_default_port_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg);
static sai_status_t mlnx_switch_vxlan_default_router_mac_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg);
static sai_status_t mlnx_switch_vxlan_default_router_mac_set(_In_ const sai_object_key_t      *key,
                                                             _In_ const sai_attribute_value_t *value,
                                                             void                             *arg);
static sai_status_t mlnx_switch_max_mirror_sessions_get(_In_ const sai_object_key_t   *key,
                                                        _Inout_ sai_attribute_value_t *value,
                                                        _In_ uint32_t                  attr_index,
                                                        _Inout_ vendor_cache_t        *cache,
                                                        void                          *arg);
static sai_status_t mlnx_switch_max_sampled_mirror_sessions_get(_In_ const sai_object_key_t   *key,
                                                                _Inout_ sai_attribute_value_t *value,
                                                                _In_ uint32_t                  attr_index,
                                                                _Inout_ vendor_cache_t        *cache,
                                                                void                          *arg);
static sai_status_t mlnx_switch_supported_stats_modes_get(_In_ const sai_object_key_t   *key,
                                                          _Inout_ sai_attribute_value_t *value,
                                                          _In_ uint32_t                  attr_index,
                                                          _Inout_ vendor_cache_t        *cache,
                                                          void                          *arg);
static sai_status_t mlnx_switch_crc_check_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg);
static sai_status_t mlnx_switch_crc_check_set(_In_ const sai_object_key_t      *key,
                                              _In_ const sai_attribute_value_t *value,
                                              void                             *arg);
static sai_status_t mlnx_switch_crc_recalculation_get(_In_ const sai_object_key_t   *key,
                                                      _Inout_ sai_attribute_value_t *value,
                                                      _In_ uint32_t                  attr_index,
                                                      _Inout_ vendor_cache_t        *cache,
                                                      void                          *arg);
static sai_status_t mlnx_switch_crc_recalculation_set(_In_ const sai_object_key_t      *key,
                                                      _In_ const sai_attribute_value_t *value,
                                                      void                             *arg);
static sai_status_t mlnx_switch_attr_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg);
static sai_status_t mlnx_switch_attr_set(_In_ const sai_object_key_t      *key,
                                         _In_ const sai_attribute_value_t *value,
                                         void                             *arg);
static sai_status_t mlnx_switch_pre_shutdown_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg);

static sai_status_t mlnx_switch_bfd_attribute_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg);
static sai_status_t mlnx_switch_bfd_event_handle(_In_ sx_trap_id_t event,
                                                 _In_ uint64_t     opaque_data);

static const sai_vendor_attribute_entry_t switch_vendor_attribs[] = {
    { SAI_SWITCH_ATTR_PORT_NUMBER,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_port_number_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_NUMBER_OF_SUPPORTED_PORTS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_ports_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_PORT_LIST,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_port_list_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_PORT_MAX_MTU,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_mtu_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_CPU_PORT,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_cpu_port_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_VIRTUAL_ROUTERS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_vr_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_FDB_TABLE_SIZE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_fdb_size_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_L3_NEIGHBOR_TABLE_SIZE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_neighbor_size_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_L3_ROUTE_TABLE_SIZE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_route_size_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ON_LINK_ROUTE_SUPPORTED,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_on_link_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_OPER_STATUS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_oper_status_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_TEMP,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_temp_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_TABLE_MINIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_table_min_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_TABLE_MAXIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_table_max_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_ENTRY_MINIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_entry_min_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_ENTRY_MAXIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_entry_max_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_TABLE_GROUP_MINIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_table_group_min_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_TABLE_GROUP_MAXIMUM_PRIORITY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_table_group_max_prio_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_FDB_DST_USER_META_DATA_RANGE,
      { false, false, false, false },
      { false, false, false, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ROUTE_DST_USER_META_DATA_RANGE,
      { false, false, false, false },
      { false, false, false, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NEIGHBOR_DST_USER_META_DATA_RANGE,
      { false, false, false, false },
      { false, false, false, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_PORT_USER_META_DATA_RANGE,
      { false, false, false, false },
      { false, false, false, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_VLAN_USER_META_DATA_RANGE,
      { false, false, false, false },
      { false, false, false, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_USER_META_DATA_RANGE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_meta_range_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_USER_TRAP_ID_RANGE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_trap_range_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_DEFAULT_VLAN_ID,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_default_vlan_id_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_DEFAULT_STP_INST_ID,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_default_stp_id_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_STP_INSTANCE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_max_stp_instance_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_TRAFFIC_CLASSES,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_qos_max_tcs_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_RESTART_TYPE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_restart_type_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MIN_PLANNED_RESTART_INTERVAL,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_min_restart_interval_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NV_STORAGE_SIZE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_nv_storage_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_LAG_MEMBERS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_lag_members_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_LAGS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_lag_number_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SWITCHING_MODE,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_mode_get, NULL,
      mlnx_switch_mode_set, NULL },
    { SAI_SWITCH_ATTR_BCAST_CPU_FLOOD_ENABLE,
      { false, false, false, false },
      { false, false, true, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MCAST_CPU_FLOOD_ENABLE,
      { false, false, false, false },
      { false, false, true, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SRC_MAC_ADDRESS,
      { false, false, false, true },
      { false, false, true, true },
      mlnx_switch_src_mac_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_LEARNED_ADDRESSES,
      { false, false, false, false },
      { false, false, true, true },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_FDB_AGING_TIME,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_aging_time_get, NULL,
      mlnx_switch_aging_time_set, NULL },
    { SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_fdb_flood_ctrl_get, (void*)SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION,
      mlnx_switch_fdb_flood_ctrl_set, (void*)SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION },
    { SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_fdb_flood_ctrl_get, (void*)SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION,
      mlnx_switch_fdb_flood_ctrl_set, (void*)SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION },
    { SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_fdb_flood_ctrl_get, (void*)SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION,
      mlnx_switch_fdb_flood_ctrl_set, (void*)SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION},
    { SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_lag_hash_attr_get, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED,
      mlnx_switch_lag_hash_attr_set, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED},
    { SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_lag_hash_attr_get, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM,
      mlnx_switch_lag_hash_attr_set, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM},
    { SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_lag_hash_attr_get, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH,
      mlnx_switch_lag_hash_attr_set, (void*)SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH},
    { SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_ecmp_hash_param_get, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED,
      mlnx_switch_ecmp_hash_param_set, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED },
    { SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_ecmp_hash_param_get, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM,
      mlnx_switch_ecmp_hash_param_set, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM },
    { SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_ecmp_hash_param_get, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH,
      mlnx_switch_ecmp_hash_param_set, (void*)SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH },
    { SAI_SWITCH_ATTR_ECMP_MEMBERS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_ecmp_members_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_ECMP_GROUPS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_ecmp_groups_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_COUNTER_REFRESH_INTERVAL,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_counter_refresh_get, NULL,
      mlnx_switch_counter_refresh_set, NULL },
    { SAI_SWITCH_ATTR_QOS_DEFAULT_TC,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_default_tc_get, NULL,
      mlnx_switch_default_tc_set, NULL },
    { SAI_SWITCH_ATTR_QOS_DOT1P_TO_TC_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_DOT1P_TO_TC,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_DOT1P_TO_TC },
    { SAI_SWITCH_ATTR_QOS_DOT1P_TO_COLOR_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_DOT1P_TO_COLOR,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_DOT1P_TO_COLOR },
    { SAI_SWITCH_ATTR_QOS_DSCP_TO_TC_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_DSCP_TO_TC,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_DSCP_TO_TC },
    { SAI_SWITCH_ATTR_QOS_DSCP_TO_COLOR_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_DSCP_TO_COLOR,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_DSCP_TO_COLOR },
    { SAI_SWITCH_ATTR_QOS_TC_TO_QUEUE_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_TC_TO_QUEUE,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_TC_TO_QUEUE },
    { SAI_SWITCH_ATTR_QOS_TC_AND_COLOR_TO_DOT1P_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DOT1P,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DOT1P },
    { SAI_SWITCH_ATTR_QOS_TC_AND_COLOR_TO_DSCP_MAP,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_qos_map_id_get, (void*)SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DSCP,
      mlnx_switch_qos_map_id_set, (void*)SAI_QOS_MAP_TYPE_TC_AND_COLOR_TO_DSCP },
    { SAI_SWITCH_ATTR_DEFAULT_TRAP_GROUP,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_default_trap_group_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_DEFAULT_VIRTUAL_ROUTER_ID,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_default_vrid_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_INGRESS_ACL,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_EGRESS_ACL,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_SCHEDULER_GROUP_HIERARCHY_LEVELS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_sched_group_levels_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_SCHEDULER_GROUPS_PER_HIERARCHY_LEVEL,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_sched_groups_count_per_level_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_QOS_MAX_NUMBER_OF_CHILDS_PER_SCHEDULER_GROUP,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_sched_max_child_groups_count_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_queue_num_get, (void*)SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_MULTICAST_QUEUES,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_queue_num_get, (void*)SAI_SWITCH_ATTR_NUMBER_OF_MULTICAST_QUEUES,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_QUEUES,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_queue_num_get, (void*)SAI_SWITCH_ATTR_NUMBER_OF_QUEUES,
      NULL, NULL },
    { SAI_SWITCH_ATTR_QOS_NUM_LOSSLESS_QUEUES,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_queue_num_get, (void*)SAI_SWITCH_ATTR_QOS_NUM_LOSSLESS_QUEUES,
      NULL, NULL },
    { SAI_SWITCH_ATTR_NUMBER_OF_CPU_QUEUES,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ECMP_HASH,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_ECMP_HASH,
      NULL, NULL },
    { SAI_SWITCH_ATTR_LAG_HASH,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_LAG_HASH,
      NULL, NULL },
    { SAI_SWITCH_ATTR_RESTART_WARM,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_restart_warm_get, NULL,
      mlnx_switch_restart_warm_set, NULL },
    { SAI_SWITCH_ATTR_WARM_RECOVER,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_warm_recover_get, NULL,
      mlnx_switch_warm_recover_set, NULL },
    { SAI_SWITCH_ATTR_ECMP_HASH_IPV4,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV4,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV4 },
    { SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4 },
    { SAI_SWITCH_ATTR_ECMP_HASH_IPV6,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV6,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_ECMP_HASH_IPV6 },
    { SAI_SWITCH_ATTR_LAG_HASH_IPV4,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV4,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV4 },
    { SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4 },
    { SAI_SWITCH_ATTR_LAG_HASH_IPV6,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_hash_object_get, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV6,
      mlnx_switch_hash_object_set, (void*)SAI_SWITCH_ATTR_LAG_HASH_IPV6 },
    { SAI_SWITCH_ATTR_TOTAL_BUFFER_SIZE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_total_pool_buffer_size_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_INGRESS_BUFFER_POOL_NUM,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_ingress_pool_num_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_EGRESS_BUFFER_POOL_NUM,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_egress_pool_num_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_INIT_SWITCH,
      { true, false, false, true },
      { true, false, false, true },
      mlnx_switch_init_connect_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SWITCH_PROFILE_ID,
      { true, false, false, true },
      { true, false, false, true },
      mlnx_switch_profile_id_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY },
    { SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY },
    { SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY },
    { SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY },
    { SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY },
    { SAI_SWITCH_ATTR_FAST_API_ENABLE,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_transaction_mode_get, NULL,
      mlnx_switch_transaction_mode_set, NULL},
    { SAI_SWITCH_ATTR_ACL_STAGE_INGRESS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_capability_get, (void*)SAI_ACL_STAGE_INGRESS,
      NULL, NULL },
    { SAI_SWITCH_ATTR_ACL_STAGE_EGRESS,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_capability_get, (void*)SAI_ACL_STAGE_EGRESS,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_ACL_ACTION_COUNT,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_acl_action_count_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_ACL_RANGE_COUNT,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_acl_range_count_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_DEFAULT_1Q_BRIDGE_ID,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_default_bridge_id_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_L2MC_ENTRY,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_IPMC_ENTRY,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE,
      NULL, NULL },
    { SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_acl_available_get, (void*)SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP,
      NULL, NULL },
    { SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_vxlan_default_port_get, NULL,
      mlnx_switch_vxlan_default_port_set, NULL },
    { SAI_SWITCH_ATTR_VXLAN_DEFAULT_ROUTER_MAC,
      { false, false, true, true },
      { false, false, true, true },
      mlnx_switch_vxlan_default_router_mac_get, NULL,
      mlnx_switch_vxlan_default_router_mac_set, NULL },
    { SAI_SWITCH_ATTR_MAX_MIRROR_SESSION,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_mirror_sessions_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_SAMPLED_MIRROR_SESSION,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_max_sampled_mirror_sessions_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SUPPORTED_EXTENDED_STATS_MODE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_supported_stats_modes_get, NULL,
      NULL, NULL },
    { SAI_SWITCH_ATTR_CRC_CHECK_ENABLE,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_crc_check_get, NULL,
      mlnx_switch_crc_check_set, NULL },
    { SAI_SWITCH_ATTR_CRC_RECALCULATION_ENABLE,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_crc_recalculation_get, NULL,
      mlnx_switch_crc_recalculation_set, NULL },
    { SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_event_func_get, (void*)SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY,
      mlnx_switch_event_func_set, (void*)SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY },
    { SAI_SWITCH_ATTR_NUMBER_OF_BFD_SESSION,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_NUMBER_OF_BFD_SESSION,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MAX_BFD_SESSION,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_MAX_BFD_SESSION,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE,
      NULL, NULL },
    { SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MIN_BFD_RX,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_MIN_BFD_RX,
      NULL, NULL },
    { SAI_SWITCH_ATTR_MIN_BFD_TX,
      { false, false, false, true },
      { false, false, false, true },
      mlnx_switch_bfd_attribute_get, (void*)SAI_SWITCH_ATTR_MIN_BFD_TX,
      NULL, NULL },
    { SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL,
      { true, false, true, true },
      { true, false, true, true },
      mlnx_switch_attr_get, (void*)SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL,
      mlnx_switch_attr_set, (void*)SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL },
    { SAI_SWITCH_ATTR_PRE_SHUTDOWN,
      { true, false, true, false },
      { true, false, true, true },
      NULL, NULL,
      mlnx_switch_pre_shutdown_set, NULL },
    { END_FUNCTIONALITY_ATTRIBS_ID,
      { false, false, false, false },
      { false, false, false, false },
      NULL, NULL,
      NULL, NULL }
};
static const mlnx_attr_enum_info_t        switch_enum_info[] = {
    [SAI_SWITCH_ATTR_OPER_STATUS] = ATTR_ENUM_VALUES_LIST(
        SAI_SWITCH_OPER_STATUS_UP),
    [SAI_SWITCH_ATTR_SWITCHING_MODE]                 = ATTR_ENUM_VALUES_ALL(),
    [SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION] = ATTR_ENUM_VALUES_LIST(
        SAI_PACKET_ACTION_FORWARD,
        SAI_PACKET_ACTION_DROP),
    [SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION] = ATTR_ENUM_VALUES_LIST(
        SAI_PACKET_ACTION_FORWARD,
        SAI_PACKET_ACTION_DROP),
    [SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION] = ATTR_ENUM_VALUES_LIST(
        SAI_PACKET_ACTION_FORWARD,
        SAI_PACKET_ACTION_DROP),
    [SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM] = ATTR_ENUM_VALUES_LIST(
        SAI_HASH_ALGORITHM_XOR,
        SAI_HASH_ALGORITHM_CRC,
        SAI_HASH_ALGORITHM_RANDOM),
    [SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM] = ATTR_ENUM_VALUES_LIST(
        SAI_HASH_ALGORITHM_XOR,
        SAI_HASH_ALGORITHM_CRC,
        SAI_HASH_ALGORITHM_RANDOM),
    [SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE] = ATTR_ENUM_VALUES_LIST(
        SAI_BFD_SESSION_OFFLOAD_TYPE_NONE),
    [SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE] = ATTR_ENUM_VALUES_LIST(
        SAI_BFD_SESSION_OFFLOAD_TYPE_NONE),
    [SAI_SWITCH_ATTR_SUPPORTED_EXTENDED_STATS_MODE] = ATTR_ENUM_VALUES_ALL(),
    [SAI_SWITCH_ATTR_RESTART_TYPE] = ATTR_ENUM_VALUES_ALL(),
};
const mlnx_obj_type_attrs_info_t          mlnx_switch_obj_type_info =
{ switch_vendor_attribs, OBJ_ATTRS_ENUMS_INFO(switch_enum_info)};

#define RDQ_ETH_DEFAULT_SIZE 4200
/* the needed value is 10000 but added more for align */
#define RDQ_ETH_LARGE_SIZE                 10240
#define RDQ_DEFAULT_NUMBER_OF_ENTRIES      1024
#define RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT 10
#define SAI_PATH                           "/sai_db"
#define SAI_QOS_PATH                       "/sai_qos_db"
#define SAI_BUFFER_PATH                    "/sai_buffer_db"
#define SAI_ACL_PATH                       "/sai_acl_db"
#define SAI_TUNNEL_PATH                    "/sai_tunnel_db"
#define SWID_NUM                           1

static struct sx_pci_profile pci_profile_single_eth_spectrum = {
    /*profile enum*/
    .pci_profile = PCI_PROFILE_EN_SINGLE_SWID,
    /*tx_prof: <swid,etclass> -> <stclass,sdq> */
    .tx_prof = {
        {
            {0, 2}, /*-0-best effort*/
            {1, 2}, /*-1-low prio*/
            {2, 2}, /*-2-medium prio*/
            {3, 2}, /*-3-*/
            {4, 2}, /*-4-*/
            {5, 1}, /*-5-high prio*/
            {6, 1}, /*-6-critical prio*/
            {6, 1} /*-7-*/
        }
    },
    /* emad_tx_prof */
    .emad_tx_prof = {
        0, 0
    },
    /* swid_type */
    .swid_type = {
        SX_KU_L2_TYPE_ETH,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE
    },
    /* rdq_count */
    .rdq_count = {
        37,     /* swid 0 */
        0,
        0,
        0,
        0,
        0,
        0,
        0
    },
    /* rdq */
    .rdq = {
        {
            /* swid 0 - ETH */
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31,
            32,
            34,
            35,
            36
        },
    },
    /* emad_rdq */
    .emad_rdq = 33,
    /* rdq_properties */
    .rdq_properties = {
        /* SWID 0 */
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-0-best effort priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-1-low priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-2-medium priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-3-high priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-4-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-5-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-6-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-7-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-8-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-9-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-10-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-11-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-12-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-13-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-14-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-15-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-16-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-17-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-18-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-19-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-20-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-21-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-22-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-23-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-24-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-25-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-26-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-27-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-28-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-29-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-30-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-31-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-32-mirror agent*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-33-emad*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-34 - special */
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-35 - special */
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-36 - special */
    },
    /* cpu_egress_tclass per SDQ */
    .cpu_egress_tclass = {
        2, /*-0-EMAD SDQ */
        1, /*-1-Control SDQ */
        0, /*-2-Data SDQ */
        0, /*-3-*/
        0, /*-4-*/
        0, /*-5-*/
        0, /*-6-*/
        0, /*-7-*/
        0, /*-8-*/
        0, /*-9-*/
        0, /*-10-*/
        0, /*-11-*/
        0, /*-12-*/
        0, /*-13-*/
        0, /*-14-*/
        0, /*-15-*/
        0, /*-16-*/
        0, /*-17-*/
        0, /*-18-*/
        0, /*-19-*/
        0, /*-20-*/
        0, /*-21-*/
        0, /*-22-*/
        0 /*-23-55*/
    },
    .dev_id = SX_DEVICE_ID
};
static struct sx_pci_profile pci_profile_single_eth_spectrum2 = {
    /*profile enum*/
    .pci_profile = PCI_PROFILE_EN_SINGLE_SWID,
    /*tx_prof: <swid,etclass> -> <stclass,sdq> */
    .tx_prof = {
        {
            {0}
        }
    },                   /* reserved */

    /* emad_tx_prof */
    .emad_tx_prof = {0}, /* reserved */

    /* swid_type */
    .swid_type = {
        SX_KU_L2_TYPE_ETH,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE
    },
    /* rdq_count */
    .rdq_count = {
        56,
        0,
        0,
        0,
        0,
        0,
        0,
        0
    },
    /* rdq */
    .rdq = {
        {
            /* swid 0 - ETH */
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31,
            32,
            34,
            35,
            36,
            37,
            38,
            39,
            40,
            41,
            42,
            43,
            44,
            45,
            46,
            47,
            48,
            49,
            50,
            51,
            52,
            53,
            54,
            55
        },
    },
    /* emad_rdq */
    .emad_rdq = 33,
    /* rdq_properties */
    .rdq_properties = {
        /* SWID 0 */
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-0-best effort priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-1-low priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-2-medium priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-3-high priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-4-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-5-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-6-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-7-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-8-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-9-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-10-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-11-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-12-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-13-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-14-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-15-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-16-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-17-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-18-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-19-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-20-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-21-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-22-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-23-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-24-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-25-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-26-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-27-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-28-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-29-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-30-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-31-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-32-mirror agent*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-33-emad*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-34-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-35-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-36-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-37-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-38-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-39-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-40-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-41-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-42-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-43-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-44-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-45-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-46-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-47-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-48-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-49-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-50-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-51-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-52-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-53-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-54-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-55-*/
    },
    /* cpu_egress_tclass per SDQ */
    .cpu_egress_tclass = {
        2, /*-0-EMAD SDQ */
        1, /*-1-Control SDQ */
        0, /*-2-Data SDQ */
        0, /*-3-*/
        0, /*-4-*/
        0, /*-5-*/
        0, /*-6-*/
        0, /*-7-*/
        0, /*-8-*/
        0, /*-9-*/
        0, /*-10-*/
        0, /*-11-*/
        0, /*-12-*/
        0, /*-13-*/
        0, /*-14-*/
        0, /*-15-*/
        0, /*-16-*/
        0, /*-17-*/
        0, /*-18-*/
        0, /*-19-*/
        0, /*-20-*/
        0, /*-21-*/
        0, /*-22-*/
        0 /*-23-55*/
    },
    .dev_id = SX_DEVICE_ID
};
struct sx_pci_profile        pci_profile_single_eth_spectrum3 = {
    /*profile enum*/
    .pci_profile = PCI_PROFILE_EN_SINGLE_SWID,
    /*tx_prof: <swid,etclass> -> <stclass,sdq> */
    .tx_prof = {
        {
            {0}
        }
    },                   /* reserved */

    /* emad_tx_prof */
    .emad_tx_prof = {0}, /* reserved */

    /* swid_type */
    .swid_type = {
        SX_KU_L2_TYPE_ETH,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE,
        SX_KU_L2_TYPE_DONT_CARE
    },
    /* rdq_count */
    .rdq_count = {
        56,
        0,
        0,
        0,
        0,
        0,
        0,
        0
    },
    /* rdq */
    .rdq = {
        {
            /* swid 0 - ETH */
            0,
            1,
            2,
            3,
            4,
            5,
            6,
            7,
            8,
            9,
            10,
            11,
            12,
            13,
            14,
            15,
            16,
            17,
            18,
            19,
            20,
            21,
            22,
            23,
            24,
            25,
            26,
            27,
            28,
            29,
            30,
            31,
            32,
            34,
            35,
            36,
            37,
            38,
            39,
            40,
            41,
            42,
            43,
            44,
            45,
            46,
            47,
            48,
            49,
            50,
            51,
            52,
            53,
            54,
            55
        },
    },
    /* emad_rdq */
    .emad_rdq = 33,
    /* rdq_properties */
    .rdq_properties = {
        /* SWID 0 */
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-0-best effort priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-1-low priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-2-medium priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0},   /*-3-high priority*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-4-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-5-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-6-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-7-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-8-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-9-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-10-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-11-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-12-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-13-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-14-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-15-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-16-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-17-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-18-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-19-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-20-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-21-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-22-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-23-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-24-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-25-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-26-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-27-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-28-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-29-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-30-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-31-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-32-mirror agent*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-33-emad*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-34-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-35-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-36-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-37-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-38-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-39-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-40-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-41-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-42-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-43-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-44-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-45-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-46-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-47-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-48-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-49-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-50-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-51-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-52-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-53-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-54-*/
        {RDQ_DEFAULT_NUMBER_OF_ENTRIES, RDQ_ETH_LARGE_SIZE, RDQ_ETH_SINGLE_SWID_DEFAULT_WEIGHT, 0}, /*-55-*/
    },
    /* cpu_egress_tclass per SDQ */
    .cpu_egress_tclass = {
        2, /*-0-EMAD SDQ */
        1, /*-1-Control SDQ */
        0, /*-2-Data SDQ */
        0, /*-3-*/
        0, /*-4-*/
        0, /*-5-*/
        0, /*-6-*/
        0, /*-7-*/
        0, /*-8-*/
        0, /*-9-*/
        0, /*-10-*/
        0, /*-11-*/
        0, /*-12-*/
        0, /*-13-*/
        0, /*-14-*/
        0, /*-15-*/
        0, /*-16-*/
        0, /*-17-*/
        0, /*-18-*/
        0, /*-19-*/
        0, /*-20-*/
        0, /*-21-*/
        0, /*-22-*/
        0 /*-23-55*/
    },
    .dev_id = SX_DEVICE_ID
};

/* device profile */
static struct ku_profile single_part_eth_device_profile_spectrum = {
    .dev_id                     = SX_DEVICE_ID,
    .set_mask_0_63              = 0x70073ff, /* bit 9 and bits 10-11 are turned off*/
    .set_mask_64_127            = 0,
    .max_vepa_channels          = 0,
    .max_lag                    = 0, /* 600,/ *TODO: PRM should define this field* / */
    .max_port_per_lag           = 0, /*TODO: PRM should define this field*/
    .max_mid                    = 0,
    .max_pgt                    = 0,
    .max_system_port            = 0, /*TODO: PRM IS NOT UPDATED*/
    .max_active_vlans           = 0,
    .max_regions                = 0,
    .max_flood_tables           = 0,
    .max_per_vid_flood_tables   = 0,
    .flood_mode                 = 3,
    .max_ib_mc                  = 0,
    .max_pkey                   = 0,
    .ar_sec                     = 0,
    .adaptive_routing_group_cap = 0,
    .arn                        = 0,
    .kvd_linear_size            = 0x10000, /* 64K */
    .kvd_hash_single_size       = 0x20000, /* 128K */
    .kvd_hash_double_size       = 0xC000, /* 24K*2 = 48K */
    .swid0_config_type          = {
        .mask = 1,
        .type = KU_SWID_TYPE_ETHERNET
    },
    .swid1_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid2_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid3_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid4_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid5_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid6_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid7_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },

    .chip_type = SXD_CHIP_TYPE_SPECTRUM,
};
struct ku_profile        single_part_eth_device_profile_spectrum2 = {
    .dev_id                     = SX_DEVICE_ID,
    .set_mask_0_63              = 0,    /* All bits are set during the init as follows:
                                         * bits 0-2   - reserved;
                                         * bit 3      - disabled (max_mid);
                                         * bits 4-7   - reserved;
                                         * bits 8     - enabled (set_flood_mode);
                                         * bits 9     - disabled (set_flood_tables);
                                         * bit 10     - disabled (set_max_fid);
                                         * bits 11-20 - reserved;
                                         * bit 21     - disabled (rfid);
                                         * bit 22     - enabled (ubridge);
                                         * bits 23-31 - reserved;
                                         * bit 32     - enabled (set cqe_version);
                                         * bits 33-63 - reserved; */
    .set_mask_64_127            = 0,    /* reserved */
    .max_vepa_channels          = 0,    /* reserved */
    .max_lag                    = 0,    /* reserved */
    .max_port_per_lag           = 0,    /* reserved */
    .max_mid                    = 0,    /* value is calculated during init with RM values */
    .max_pgt                    = 0,    /* reserved */
    .max_system_port            = 0,    /* reserved */
    .max_active_vlans           = 0,    /* reserved */
    .max_regions                = 0,    /* reserved */
    .max_flood_tables           = 0,
    .max_per_vid_flood_tables   = 0,
    .flood_mode                 = 4,
    .max_ib_mc                  = 0,    /* reserved */
    .max_pkey                   = 0,    /* reserved */
    .ar_sec                     = 0,    /* reserved */
    .adaptive_routing_group_cap = 0,    /* reserved */
    .arn                        = 0,    /* reserved */
    /* .ubridge_mode = 0,                  / * reserved * / */
    /* .kvd_linear_size = 0,               / * reserved * / */
    .kvd_hash_single_size = 0,          /* reserved */
    .kvd_hash_double_size = 0,          /* reserved */
    .swid0_config_type    = {
        .mask = 1,
        .type = KU_SWID_TYPE_ETHERNET
    },
    .swid1_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid2_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid3_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid4_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid5_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid6_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid7_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },

    .chip_type = SXD_CHIP_TYPE_SPECTRUM2,
};
struct ku_profile        single_part_eth_device_profile_spectrum3 = {
    .dev_id                     = SX_DEVICE_ID,
    .set_mask_0_63              = 0,    /* All bits are set during the init as follows:
                                         * bits 0-2   - reserved;
                                         * bit 3      - disabled (max_mid);
                                         * bits 4-7   - reserved;
                                         * bits 8     - enabled (set_flood_mode);
                                         * bits 9     - disabled (set_flood_tables);
                                         * bit 10     - disabled (set_max_fid);
                                         * bits 11-20 - reserved;
                                         * bit 21     - disabled (rfid);
                                         * bit 22     - enabled (ubridge);
                                         * bits 23-31 - reserved;
                                         * bit 32     - enabled (set cqe_version);
                                         * bits 33-63 - reserved; */
    .set_mask_64_127            = 0,    /* reserved */
    .max_vepa_channels          = 0,    /* reserved */
    .max_lag                    = 0,    /* reserved */
    .max_port_per_lag           = 0,    /* reserved */
    .max_mid                    = 0,    /* value is calculated during init with RM values */
    .max_pgt                    = 0,    /* reserved */
    .max_system_port            = 0,    /* reserved */
    .max_active_vlans           = 0,    /* reserved */
    .max_regions                = 0,    /* reserved */
    .max_flood_tables           = 0,
    .max_per_vid_flood_tables   = 0,
    .flood_mode                 = 4,
    .max_ib_mc                  = 0,    /* reserved */
    .max_pkey                   = 0,    /* reserved */
    .ar_sec                     = 0,    /* reserved */
    .adaptive_routing_group_cap = 0,    /* reserved */
    .arn                        = 0,    /* reserved */
    .ubridge_mode               = 0,    /* reserved */
    .kvd_linear_size            = 0,    /* reserved */
    .kvd_hash_single_size       = 0,    /* reserved */
    .kvd_hash_double_size       = 0,    /* reserved */
    .swid0_config_type          = {
        .mask = 1,
        .type = KU_SWID_TYPE_ETHERNET
    },
    .swid1_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid2_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid3_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid4_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid5_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid6_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },
    .swid7_config_type = {
        .mask = 1,
        .type = KU_SWID_TYPE_DISABLED
    },

    .chip_type = SXD_CHIP_TYPE_SPECTRUM3,
};


#ifdef CONFIG_SYSLOG
void log_cb(sx_log_severity_t severity, const char *module_name, char *msg)
{
    if (!g_log_init) {
        openlog("SDK", 0, LOG_USER);
        g_log_init = true;
    }

    mlnx_syslog(severity, module_name, "%s", msg);
}

void log_pause_cb(void)
{
    closelog();
    g_log_init = false;
}
#else
void log_cb(sx_log_severity_t severity, const char *module_name, char *msg)
{
    UNREFERENCED_PARAMETER(severity);
    UNREFERENCED_PARAMETER(module_name);
    UNREFERENCED_PARAMETER(msg);
}
#endif /* CONFIG_SYSLOG */

static void switch_key_to_str(_In_ sai_object_id_t switch_id, _Out_ char *key_str)
{
    mlnx_object_id_t mlnx_switch_id = { 0 };
    sai_status_t     status;

    status = sai_to_mlnx_object_id(SAI_OBJECT_TYPE_SWITCH, switch_id, &mlnx_switch_id);
    if (SAI_ERR(status)) {
        snprintf(key_str, MAX_KEY_STR_LEN, "Invalid Switch ID");
    } else {
        snprintf(key_str, MAX_KEY_STR_LEN, "Switch ID %u", mlnx_switch_id.id.is_created);
    }
}

/* SAI DB must be inited */
static sx_api_pci_profile_t* mlnx_sai_get_pci_profile(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    switch (chip_type) {
    case SX_CHIP_TYPE_SPECTRUM:
    case SX_CHIP_TYPE_SPECTRUM_A1:
        return &pci_profile_single_eth_spectrum;

    case SX_CHIP_TYPE_SPECTRUM2:
        return &pci_profile_single_eth_spectrum2;

    case SX_CHIP_TYPE_SPECTRUM3:
        return &pci_profile_single_eth_spectrum3;

    default:
        MLNX_SAI_LOG_ERR("g_sai_db_ptr->sxd_chip_type = %s\n", SX_CHIP_TYPE_STR(chip_type));
        return NULL;
    }

    return NULL;
}

/* SAI DB must be inited */
static sx_api_profile_t* mlnx_sai_get_ku_profile(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    switch (chip_type) {
    case SX_CHIP_TYPE_SPECTRUM:
    case SX_CHIP_TYPE_SPECTRUM_A1:
        return &single_part_eth_device_profile_spectrum;

    case SX_CHIP_TYPE_SPECTRUM2:
        return &single_part_eth_device_profile_spectrum2;

    case SX_CHIP_TYPE_SPECTRUM3:
        return &single_part_eth_device_profile_spectrum3;

    default:
        MLNX_SAI_LOG_ERR("g_sai_db_ptr->sxd_chip_type = %s\n", SX_CHIP_TYPE_STR(chip_type));
        return NULL;
    }

    return NULL;
}

static sai_status_t mlnx_switch_bfd_attribute_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg)
{
    mlnx_object_id_t mlnx_switch_id = {0};
    sai_status_t     status;
    uint64_t         type = (uint64_t)arg;

    assert(type == SAI_SWITCH_ATTR_NUMBER_OF_BFD_SESSION ||
           type == SAI_SWITCH_ATTR_MAX_BFD_SESSION ||
           type == SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE ||
           type == SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE ||
           type == SAI_SWITCH_ATTR_MIN_BFD_RX ||
           type == SAI_SWITCH_ATTR_MIN_BFD_TX);

    SX_LOG_ENTER();

    status = sai_to_mlnx_object_id(SAI_OBJECT_TYPE_SWITCH, key->key.object_id, &mlnx_switch_id);
    if (SAI_ERR(status)) {
        SX_LOG_EXIT();
        return status;
    }

    switch(type) {
    case SAI_SWITCH_ATTR_NUMBER_OF_BFD_SESSION:
        sai_db_read_lock();
        value->u32 = MAX_BFD_SESSION_NUMBER - mlnx_shm_rm_array_free_entries_count(MLNX_SHM_RM_ARRAY_TYPE_BFD_SESSION);
        sai_db_unlock();
        break;

    case SAI_SWITCH_ATTR_MAX_BFD_SESSION:
        value->u32 = MAX_BFD_SESSION_NUMBER;
        break;

    case SAI_SWITCH_ATTR_SUPPORTED_IPV4_BFD_SESSION_OFFLOAD_TYPE:
    case SAI_SWITCH_ATTR_SUPPORTED_IPV6_BFD_SESSION_OFFLOAD_TYPE:
        value->u32 = SAI_BFD_SESSION_OFFLOAD_TYPE_NONE;
        break;

    case SAI_SWITCH_ATTR_MIN_BFD_RX:
    case SAI_SWITCH_ATTR_MIN_BFD_TX:
        value->u32 = BFD_MIN_SUPPORTED_INTERVAL;
        break;
    default:
        SX_LOG_ERR("Unexpected arg type: %lu\n", type);
        SX_LOG_EXIT();
        return SAI_STATUS_FAILURE;
    }

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

uint8_t mlnx_port_mac_mask_get(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    switch (chip_type) {
    case SX_CHIP_TYPE_SPECTRUM:
    case SX_CHIP_TYPE_SPECTRUM_A1:
        return PORT_MAC_BITMASK_SP;

    case SX_CHIP_TYPE_SPECTRUM2:
    case SX_CHIP_TYPE_SPECTRUM3:
        return PORT_MAC_BITMASK_SP2_3;

    default:
        MLNX_SAI_LOG_ERR("g_sai_db_ptr->sx_chip_type = %s\n", SX_CHIP_TYPE_STR(chip_type));
        return 0;
    }

    return 0;
}

static sai_status_t mlnx_sai_db_initialize(const char *config_file, sx_chip_types_t chip_type)
{
    sai_status_t status = SAI_STATUS_FAILURE;

    if (SAI_STATUS_SUCCESS != (status = sai_db_create())) {
        return status;
    }

    if (SAI_STATUS_SUCCESS != (status = sai_qos_db_create())) {
        return status;
    }

    sai_db_values_init();

    g_sai_db_ptr->sx_chip_type = chip_type;

    if (SAI_STATUS_SUCCESS != (status = mlnx_parse_config(config_file))) {
        return status;
    }

    if (SAI_STATUS_SUCCESS != (status = sai_buffer_db_create())) {
        return status;
    }
    sai_buffer_db_values_init();

    if (SAI_STATUS_SUCCESS != (status = sai_acl_db_create())) {
        return status;
    }
    sai_acl_db_init();

    if (SAI_STATUS_SUCCESS != (status = sai_tunnel_db_create())) {
        return status;
    }
    sai_tunnel_db_init();

    status = mlnx_sai_rm_db_init();
    if (SAI_ERR(status)) {
        return status;
    }

    status = mlnx_debug_counter_db_init();
    if (SAI_ERR(status)) {
        return status;
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_resource_mng_stage(bool warm_recover, mlnx_sai_boot_type_t boot_type)
{
    sxd_status_t              sxd_ret = SXD_STATUS_SUCCESS;
    sxd_ctrl_pack_t           ctrl_pack;
    struct ku_dpt_path_add    path;
    struct ku_dpt_path_modify path_modify;
    struct ku_swid_details    swid_details;
    sx_api_pci_profile_t     *pci_profile;
    char                      dev_name[MAX_NAME_LEN];
    char                     *dev_names[1] = { dev_name };
    uint32_t                  dev_num      = 1;
    sxd_handle                sxd_handle   = 0;
    uint32_t                  ii;
    const bool                initialize_dpt = !warm_recover;
    const bool                reset_asic     = !warm_recover && (BOOT_TYPE_WARM != boot_type);

    memset(&ctrl_pack, 0, sizeof(sxd_ctrl_pack_t));
    memset(&swid_details, 0, sizeof(swid_details));
    memset(&path_modify, 0, sizeof(path_modify));

    /* sxd_dpt_init will destroy existing dpt shared memory and create new one.
     * For warmboot we want to keep the old shared memory before reboot */
    if (initialize_dpt) {
        sxd_ret = sxd_dpt_init(SYS_TYPE_EN, sai_log_cb, LOG_VAR_NAME(__MODULE__));
        if (SXD_CHECK_FAIL(sxd_ret)) {
            MLNX_SAI_LOG_ERR("Failed to init dpt - %s.\n", SXD_STATUS_MSG(sxd_ret));
            return SAI_STATUS_FAILURE;
        }
    }

    sxd_ret = sxd_dpt_set_access_control(SX_DEVICE_ID, READ_WRITE);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("Failed to set dpt access control - %s.\n", SXD_STATUS_MSG(sxd_ret));
        return SAI_STATUS_FAILURE;
    }

    sxd_ret = sxd_access_reg_init(0, sai_log_cb, LOG_VAR_NAME(__MODULE__));
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("Failed to init access reg - %s.\n", SXD_STATUS_MSG(sxd_ret));
        return SAI_STATUS_FAILURE;
    }

    /* get device list from the devices directory */
    sxd_ret = sxd_get_dev_list(dev_names, &dev_num);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("sxd_get_dev_list error %s.\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    /* open the first device */
    sxd_ret = sxd_open_device(dev_name, &sxd_handle);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("sxd_open_device error %s.\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd = CTRL_CMD_ADD_DEV_PATH;
    ctrl_pack.cmd_body = (void*)&(path);
    memset(&path, 0, sizeof(struct ku_dpt_path_add));
    path.dev_id                           = SX_DEVICE_ID;
    path.path_type                        = DPT_PATH_I2C;
    path.path_info.sx_i2c_info.sx_i2c_dev = 0x420248;
    sxd_ret                               = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to add I2C dev path to DP table, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd = CTRL_CMD_ADD_DEV_PATH;
    ctrl_pack.cmd_body = (void*)&(path);
    memset(&path, 0, sizeof(struct ku_dpt_path_add));
    path.dev_id                        = SX_DEVICE_ID;
    path.path_type                     = DPT_PATH_PCI_E;
    path.path_info.sx_pcie_info.pci_id = 256;
    path.is_local                      = 1;
    sxd_ret                            = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to add PCI dev path to DP table, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd    = CTRL_CMD_SET_CMD_PATH;
    ctrl_pack.cmd_body    = (void*)&(path_modify);
    path_modify.dev_id    = SX_DEVICE_ID;
    path_modify.path_type = DPT_PATH_PCI_E;
    sxd_ret               = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to set cmd_ifc path in DP table to PCI, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd = CTRL_CMD_SET_EMAD_PATH;
    sxd_ret            = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to set emad path in DP table to PCI, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd = CTRL_CMD_SET_MAD_PATH;
    sxd_ret            = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to set mad path in DP table to PCI, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    ctrl_pack.ctrl_cmd = CTRL_CMD_SET_CR_ACCESS_PATH;
    sxd_ret            = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to set cr access path in DP table to PCI, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    if (reset_asic) {
        ctrl_pack.ctrl_cmd = CTRL_CMD_RESET;
        ctrl_pack.cmd_body = (void*)SX_DEVICE_ID;
        sxd_ret            = sxd_ioctl(sxd_handle, &ctrl_pack);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            MLNX_SAI_LOG_ERR("failed to reset asic, error: %s\n", strerror(errno));
            return SAI_STATUS_FAILURE;
        }

        pci_profile = mlnx_sai_get_pci_profile();
        if (!pci_profile) {
            return SAI_STATUS_FAILURE;
        }

        ctrl_pack.ctrl_cmd = CTRL_CMD_SET_PCI_PROFILE;
        ctrl_pack.cmd_body = (void*)pci_profile;
        sxd_ret            = sxd_ioctl(sxd_handle, &ctrl_pack);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            MLNX_SAI_LOG_ERR("failed to set pci profile in asic, error: %s\n", strerror(errno));
            return SAI_STATUS_FAILURE;
        }

        /* enable device's swid */
        swid_details.dev_id = SX_DEVICE_ID;
        ctrl_pack.cmd_body  = (void*)&(swid_details);
        ctrl_pack.ctrl_cmd  = CTRL_CMD_ENABLE_SWID;
        for (ii = 0; ii < SWID_NUM; ++ii) {
            swid_details.swid        = ii;
            swid_details.iptrap_synd = SXD_TRAP_ID_IPTRAP_MIN + ii;
            cl_plock_acquire(&g_sai_db_ptr->p_lock);
            swid_details.mac = SX_MAC_TO_U64(g_sai_db_ptr->base_mac_addr);
            cl_plock_release(&g_sai_db_ptr->p_lock);

            sxd_ret = sxd_ioctl(sxd_handle, &ctrl_pack);
            if (SXD_CHECK_FAIL(sxd_ret)) {
                MLNX_SAI_LOG_ERR("failed to enable swid %u : %s\n", ii, strerror(errno));
                return SAI_STATUS_FAILURE;
            }
        }
    }

#ifdef PTP
    struct ku_ptp_mode ku_ptp_mode;
    memset(&ku_ptp_mode, 0, sizeof(ku_ptp_mode));
    ku_ptp_mode.ptp_mode = KU_PTP_MODE_EVENTS;
    ctrl_pack.ctrl_cmd   = CTRL_CMD_SET_PTP_MODE;
    ctrl_pack.cmd_body   = &ku_ptp_mode;
    sxd_ret              = sxd_ioctl(sxd_handle, &ctrl_pack);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("failed to add I2C dev path to DP table, error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }
#endif

    sxd_ret = sxd_close_device(sxd_handle);
    if (SXD_CHECK_FAIL(sxd_ret)) {
        MLNX_SAI_LOG_ERR("sxd_close_device error: %s\n", strerror(errno));
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_wait_for_sdk(const char *sdk_ready_var)
{
    const double time_unit = 0.001;

    while (0 != access(sdk_ready_var, F_OK)) {
#ifndef _WIN32
        usleep(time_unit);
#endif
    }

    return SAI_STATUS_SUCCESS;
}

static sx_status_t get_chip_type(enum sxd_chip_types* chip_type)
{
    uint16_t device_hw_revision;
    uint16_t device_id;
    FILE   * f = NULL;
    int      rc;

#ifdef _WIN32
#define SCNu16 "u"
#endif

    f = fopen("/sys/module/sx_core/parameters/chip_info_type", "r");
    if (f == NULL) {
        MLNX_SAI_LOG_ERR("failed to open /sys/module/sx_core/parameters/chip_info_type\n");
        return SX_STATUS_ERROR;
    }

    rc = fscanf(f, "%" SCNu16, &device_id);
    fclose(f);

    if (rc != 1) {
        MLNX_SAI_LOG_ERR("failed to open /sys/module/sx_core/parameters/chip_info_type\n");
        return SX_STATUS_ERROR;
    }

    f = fopen("/sys/module/sx_core/parameters/chip_info_revision", "r");
    if (f == NULL) {
        MLNX_SAI_LOG_ERR("failed to open /sys/module/sx_core/parameters/chip_info_revision\n");
        return SX_STATUS_ERROR;
    }

    rc = fscanf(f, "%" SCNu16, &device_hw_revision);
    fclose(f);

    if (rc != 1) {
        MLNX_SAI_LOG_ERR("failed to open /sys/module/sx_core/parameters/chip_info_revision\n");
        return SX_STATUS_ERROR;
    }

    switch (device_id) {
    case SXD_MGIR_HW_DEV_ID_SPECTRUM:
        if (device_hw_revision == 0xA0) {
            *chip_type = SXD_CHIP_TYPE_SPECTRUM;
        } else if (device_hw_revision == 0xA1) {
            *chip_type = SXD_CHIP_TYPE_SPECTRUM_A1;
        } else {
            SX_LOG_ERR("Unsupported spectrum revision %u\n", device_hw_revision);
            return SX_STATUS_ERROR;
        }
        break;

    case SXD_MGIR_HW_DEV_ID_SPECTRUM2:
        *chip_type = SXD_CHIP_TYPE_SPECTRUM2;
        break;

    case SXD_MGIR_HW_DEV_ID_SPECTRUM3:
        *chip_type = SXD_CHIP_TYPE_SPECTRUM3;
        break;

    default:
        MLNX_SAI_LOG_ERR("Unsupported device %u %u\n", device_id, device_hw_revision);
        return SX_STATUS_ERROR;
    }

    return SX_STATUS_SUCCESS;
}

bool mlnx_chip_is_spc(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    assert(chip_type != SX_CHIP_TYPE_UNKNOWN);

    return (chip_type == SX_CHIP_TYPE_SPECTRUM) || (chip_type == SX_CHIP_TYPE_SPECTRUM_A1);
}

bool mlnx_chip_is_spc2(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    assert(chip_type != SX_CHIP_TYPE_UNKNOWN);

    return chip_type == SX_CHIP_TYPE_SPECTRUM2;
}

bool mlnx_chip_is_spc3(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    assert(chip_type != SX_CHIP_TYPE_UNKNOWN);

    return chip_type == SX_CHIP_TYPE_SPECTRUM3;
}

bool mlnx_chip_is_spc2or3(void)
{
    sx_chip_types_t chip_type = g_sai_db_ptr->sx_chip_type;

    assert(chip_type != SX_CHIP_TYPE_UNKNOWN);

    return (chip_type == SX_CHIP_TYPE_SPECTRUM2) || (chip_type == SX_CHIP_TYPE_SPECTRUM3);
}

mlnx_platform_type_t mlnx_platform_type_get(void)
{
    return g_sai_db_ptr->platform_type;
}

static sai_status_t mlnx_sdk_start(mlnx_sai_boot_type_t boot_type)
{
    sai_status_t sai_status;
    int          system_err, cmd_len;
    char         sdk_start_cmd[SDK_START_CMD_STR_LEN] = {0};
    const char  *sniffer_var                          = NULL;
    const char  *sniffer_cmd                          = "";
    const char  *vlagrind_cmd                         = "";
    const char  *syslog_cmd                           = "";
    const char  *fastboot_cmd                         = "";
    const char  *sdk_ready_var                        = NULL;
    char         sdk_ready_cmd[SDK_START_CMD_STR_LEN] = {0};

    sdk_ready_var = getenv("SDK_READY_FILE");
    if (!sdk_ready_var || (0 == strcmp(sdk_ready_var, ""))) {
        sdk_ready_var = "/tmp/sdk_ready";
    }
    cmd_len = snprintf(sdk_ready_cmd,
                       SDK_START_CMD_STR_LEN,
                       "rm %s",
                       sdk_ready_var);
    assert(cmd_len < SDK_START_CMD_STR_LEN);

    system_err = system(sdk_ready_cmd);
    if (0 == system_err) {
        MLNX_SAI_LOG_DBG("%s removed\n", sdk_ready_var);
    } else {
        MLNX_SAI_LOG_DBG("unable to remove %s\n", sdk_ready_var);
    }

    sniffer_var = getenv("SX_SNIFFER_ENABLE");
    if (sniffer_var && (0 == strcmp(sniffer_var, "1"))) {
        sniffer_cmd = "LD_PRELOAD=\"libsxsniffer.so\"";
    }

#ifdef SDK_VALGRIND
    vlagrind_cmd = "valgrind --tool=memcheck --leak-check=full --error-exitcode=1 --undef-value-errors=no "
                   "--run-libc-freeres=yes --max-stackframe=15310736";
#endif
#ifdef CONFIG_SYSLOG
    syslog_cmd = "--logger libsai.so";
#endif
    if ((BOOT_TYPE_WARM == boot_type) || (BOOT_TYPE_FAST == boot_type)) {
        fastboot_cmd = "env FAST_BOOT=1";
    }

    cmd_len = snprintf(sdk_start_cmd,
                       SDK_START_CMD_STR_LEN,
                       "%s %s %s sx_sdk %s &",
                       sniffer_cmd,
                       vlagrind_cmd,
                       fastboot_cmd,
                       syslog_cmd);
    assert(cmd_len < SDK_START_CMD_STR_LEN);

    system_err = system(sdk_start_cmd);
    if (0 != system_err) {
        MLNX_SAI_LOG_ERR("Failed running sx_sdk\n");
        return SAI_STATUS_FAILURE;
    }

    sai_status = mlnx_wait_for_sdk(sdk_ready_var);
    assert(SAI_STATUS_SUCCESS == sai_status);

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_chassis_mng_stage(mlnx_sai_boot_type_t boot_type,
                                           bool                 transaction_mode_enable,
                                           sx_api_profile_t    *ku_profile)
{
    sx_status_t           status;
    sx_api_sx_sdk_init_t  sdk_init_params;
    sx_api_pci_profile_t *pci_profile;
    uint32_t              bridge_acls = 0;
    uint8_t               port_phy_bits_num;
    uint8_t               port_pth_bits_num;
    uint8_t               port_sub_bits_num;
    sx_access_cmd_t       transaction_mode_cmd = SX_ACCESS_CMD_NONE;
    const char           *issu_path;

    if (NULL == ku_profile) {
        return SAI_STATUS_FAILURE;
    }

    memset(&sdk_init_params, 0, sizeof(sdk_init_params));

    /* Open an handle */
    if (SX_STATUS_SUCCESS != (status = sx_api_open(sai_log_cb, &gh_sdk))) {
        MLNX_SAI_LOG_ERR("Can't open connection to SDK - %s.\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    sdk_init_params.app_id = htonl(*((uint32_t*)"SDK1"));

    sdk_init_params.policer_params.priority_group_num = 3;

    sdk_init_params.port_params.max_dev_id = SX_DEV_ID_MAX;

    sdk_init_params.topo_params.max_num_of_tree_per_chip = 18; /* max num of trees */

    sdk_init_params.lag_params.max_ports_per_lag = 0;
    sdk_init_params.lag_params.default_lag_hash  = SX_LAG_DEFAULT_LAG_HASH;

    sdk_init_params.vlan_params.def_vid     = SX_VLAN_DEFAULT_VID;
    sdk_init_params.vlan_params.max_swid_id = SX_SWID_ID_MAX;

    sdk_init_params.fdb_params.max_mc_group = SX_FDB_MAX_MC_GROUPS;
    sdk_init_params.fdb_params.flood_mode   = FLOOD_PER_VLAN;

    sdk_init_params.mstp_params.mode = SX_MSTP_MODE_RSTP;

    sdk_init_params.router_profile_params.min_router_counters = 16;

    sdk_init_params.acl_params.max_swid_id = 0;

    sdk_init_params.flow_counter_params.flow_counter_byte_type_min_number   = 0;
    sdk_init_params.flow_counter_params.flow_counter_packet_type_min_number = 0;
    sdk_init_params.flow_counter_params.flow_counter_byte_type_max_number   = ACL_MAX_SX_COUNTER_BYTE_NUM;
    sdk_init_params.flow_counter_params.flow_counter_packet_type_max_number = ACL_MAX_SX_COUNTER_PACKET_NUM;

    /* ISSU: Need to cut MAX ACL groups by half, and share with ingress and egress */
    if (g_sai_db_ptr->issu_enabled) {
        sdk_init_params.acl_params.max_acl_ingress_groups = ACL_MAX_SX_ING_GROUP_NUMBER / 4;
        sdk_init_params.acl_params.max_acl_egress_groups  = ACL_MAX_SX_EGR_GROUP_NUMBER / 4;
    } else {
        sdk_init_params.acl_params.max_acl_ingress_groups = ACL_MAX_SX_ING_GROUP_NUMBER;
        sdk_init_params.acl_params.max_acl_egress_groups  = ACL_MAX_SX_EGR_GROUP_NUMBER;
    }

    sdk_init_params.acl_params.min_acl_rules = 0;
    sdk_init_params.acl_params.max_acl_rules = ACL_MAX_SX_RULES_NUMBER;
    /* TODO : Change to parallel after bmtor fix */
    sdk_init_params.acl_params.acl_search_type = SX_API_ACL_SEARCH_TYPE_SERIAL; /* mlnx_chip_is_spc2() ? */
    /* SX_API_ACL_SEARCH_TYPE_SERIAL : SX_API_ACL_SEARCH_TYPE_SERIAL; */

    sdk_init_params.bridge_init_params.sdk_mode                                   = SX_MODE_HYBRID;
    sdk_init_params.bridge_init_params.sdk_mode_params.mode_hybrid.max_bridge_num = MAX_BRIDGES_1D;
    /* correct the min/max acls according to bridge requirments */
    /* number for homgenous mode egress rules */
    bridge_acls = sdk_init_params.bridge_init_params.sdk_mode_params.mode_1D.max_bridge_num;
    /* number for ingress rules */
    bridge_acls += sdk_init_params.bridge_init_params.sdk_mode_params.mode_1D.max_virtual_ports_num;

    sdk_init_params.acl_params.max_acl_rules += bridge_acls;

    port_phy_bits_num = SX_PORT_UCR_ID_PHY_NUM_OF_BITS;
    port_pth_bits_num = 16;
    port_sub_bits_num = 0;

    if (sdk_init_params.port_params.port_phy_bits_num == 0) {
        sdk_init_params.port_params.port_phy_bits_num = port_phy_bits_num;
    }
    if (sdk_init_params.port_params.port_pth_bits_num == 0) {
        sdk_init_params.port_params.port_pth_bits_num = port_pth_bits_num;
    }
    if (sdk_init_params.port_params.port_sub_bits_num == 0) {
        sdk_init_params.port_params.port_sub_bits_num = port_sub_bits_num;
    }

    pci_profile = mlnx_sai_get_pci_profile();
    if (!pci_profile) {
        return SAI_STATUS_FAILURE;
    }

    memcpy(&(sdk_init_params.profile), ku_profile, sizeof(struct ku_profile));
    memcpy(&(sdk_init_params.pci_profile), pci_profile, sizeof(struct sx_pci_profile));
    sdk_init_params.applibs_mask = SX_API_FLOW_COUNTER | SX_API_POLICER | SX_API_HOST_IFC | SX_API_SPAN |
                                   SX_API_ETH_L2 | SX_API_ACL;
    sdk_init_params.profile.chip_type = g_sai_db_ptr->sx_chip_type;

    issu_path = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_WARM_BOOT_WRITE_FILE);
    if (NULL != issu_path) {
        strncpy(sdk_init_params.boot_mode_params.issu_persistent_path, issu_path, SX_ISSU_PATH_LEN_MAX - 1);
    }
    sdk_init_params.boot_mode_params.issu_persistent_path[SX_ISSU_PATH_LEN_MAX - 1] = 0;

    switch (boot_type) {
    case BOOT_TYPE_REGULAR:
        if (g_sai_db_ptr->issu_enabled) {
            sdk_init_params.boot_mode_params.boot_mode = SX_BOOT_MODE_ISSU_NORMAL_E;
        } else {
            sdk_init_params.boot_mode_params.boot_mode = SX_BOOT_MODE_NORMAL_E;
        }
        break;

    case BOOT_TYPE_WARM:
        if (g_sai_db_ptr->issu_enabled) {
            sdk_init_params.boot_mode_params.boot_mode         = SX_BOOT_MODE_ISSU_STARTED_E;
            sdk_init_params.boot_mode_params.pdb_lag_init      = true;
            sdk_init_params.boot_mode_params.pdb_port_map_init = true;
        } else {
            SX_LOG_ERR("issu-enabled flag in xml file should be enabled for fast fast boot\n");
            return SAI_STATUS_FAILURE;
        }
        break;

    case BOOT_TYPE_FAST:
        if (g_sai_db_ptr->issu_enabled) {
            sdk_init_params.boot_mode_params.boot_mode = SX_BOOT_MODE_ISSU_FAST_E;
        } else {
            sdk_init_params.boot_mode_params.boot_mode = SX_BOOT_MODE_FAST_E;
        }
        break;

    default:
        SX_LOG_ERR("Unsupported boot type %d\n", boot_type);
        return SAI_STATUS_FAILURE;
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_sdk_init_set(gh_sdk, &sdk_init_params))) {
        SX_LOG_ERR("Failed to initialize SDK (%s)\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    SX_LOG_NTC("SDK initialized successfully\n");

    /* transaction mode is disabled by default */
    if (transaction_mode_enable) {
        transaction_mode_cmd = SX_ACCESS_CMD_ENABLE;
        if (SX_STATUS_SUCCESS !=
            (status = sx_api_transaction_mode_set(gh_sdk, transaction_mode_cmd))) {
            MLNX_SAI_LOG_ERR("Failed to set transaction mode to %d: %s\n", transaction_mode_cmd,
                             SX_STATUS_MSG(status));
            return sdk_to_sai(status);
        }
    }

    status = mlnx_sai_log_levels_post_init();
    if (SAI_ERR(status)) {
        return status;
    }

    return SAI_STATUS_SUCCESS;
}

#ifndef _WIN32
static sai_status_t parse_port_info(xmlDoc *doc, xmlNode * port_node)
{
    bool                            local_found    = false;
    bool                            width_found    = false;
    bool                            module_found   = false;
    bool                            breakout_found = false;
    bool                            speed_found    = false;
    uint32_t                        local          = 0;
    uint32_t                        module         = 0;
    uint32_t                        width          = 0;
    mlnx_port_breakout_capability_t breakout_modes = MLNX_PORT_BREAKOUT_CAPABILITY_NONE;
    uint32_t                        split_count    = 0;
    sx_port_speed_t                 port_speed     = 0;
    mlnx_port_config_t             *tmp_port;
    mlnx_port_config_t             *port;
    xmlChar                        *key;
    uint32_t                        ii;

    if (g_sai_db_ptr->ports_configured >= MAX_PORTS) {
        MLNX_SAI_LOG_ERR("Ports configured %u bigger than max %u\n", g_sai_db_ptr->ports_configured, MAX_PORTS);
        return SAI_STATUS_FAILURE;
    }

    while (port_node != NULL) {
        if ((!xmlStrcmp(port_node->name, (const xmlChar*)"local-port"))) {
            key         = xmlNodeListGetString(doc, port_node->xmlChildrenNode, 1);
            local       = (uint32_t)atoi((const char*)key);
            local_found = true;
            xmlFree(key);
        } else if ((!xmlStrcmp(port_node->name, (const xmlChar*)"width"))) {
            key         = xmlNodeListGetString(doc, port_node->children, 1);
            width       = (uint32_t)atoi((const char*)key);
            width_found = true;
            xmlFree(key);
        } else if ((!xmlStrcmp(port_node->name, (const xmlChar*)"module"))) {
            key          = xmlNodeListGetString(doc, port_node->children, 1);
            module       = (uint32_t)atoi((const char*)key);
            module_found = true;
            xmlFree(key);
        } else if ((!xmlStrcmp(port_node->name, (const xmlChar*)"breakout-modes"))) {
            key            = xmlNodeListGetString(doc, port_node->children, 1);
            breakout_modes = (uint32_t)atoi((const char*)key);
            breakout_found = true;
            xmlFree(key);
        } else if ((!xmlStrcmp(port_node->name, (const xmlChar*)"port-speed"))) {
            key         = xmlNodeListGetString(doc, port_node->children, 1);
            port_speed  = (uint32_t)atoi((const char*)key);
            speed_found = true;
            xmlFree(key);
        } else if ((!xmlStrcmp(port_node->name, (const xmlChar*)"split"))) {
            key         = xmlNodeListGetString(doc, port_node->children, 1);
            split_count = (uint32_t)atoi((const char*)key);
            xmlFree(key);

            if ((split_count != 1) && (split_count != 2) && (split_count != 4)) {
                MLNX_SAI_LOG_ERR("Port <split> value (%u) - only 1,2 or 4 are supported\n",
                                 split_count);
                return SAI_STATUS_FAILURE;
            }
        }

        port_node = port_node->next;
    }

    if (!local_found || !width_found || !module_found || !breakout_found || !speed_found) {
        MLNX_SAI_LOG_ERR("missing port data %u local %u width %u module %u breakout %u speed %u\n",
                         g_sai_db_ptr->ports_configured,
                         local_found,
                         width_found,
                         module_found,
                         breakout_found,
                         speed_found);
        return SAI_STATUS_FAILURE;
    }

    /* It is required by PTF tests that ports must be ordered in the same way like
     * they are mapped via XML file, so we just swap local id parsed from
     * XML with a port from DB with same local id */
    port                          = mlnx_port_by_idx(g_sai_db_ptr->ports_configured);
    tmp_port                      = mlnx_port_by_local_id(local);
    tmp_port->port_map.local_port = port->port_map.local_port;

    port->breakout_modes = breakout_modes;
    port->split_count    = split_count;
    port->speed_bitmap   = port_speed;
    port->module         = module;
    port->width          = width;
    port->is_present     = true;

    port->port_map.mapping_mode = SX_PORT_MAPPING_MODE_ENABLE;
    port->port_map.module_port  = module;
    port->port_map.width        = width;
    port->port_map.config_hw    = FALSE;
    port->port_map.lane_bmap    = 0x0;
    port->port_map.local_port   = local;

    for (ii = 0; ii < width; ii++) {
        port->port_map.lane_bmap |= 1 << ii;
    }

    g_sai_db_ptr->ports_configured++;

    MLNX_SAI_LOG_NTC("Port %u {local=%u module=%u width=%u lanes=0x%x breakout-modes=%u split=%u, port-speed=%u}\n",
                     g_sai_db_ptr->ports_configured,
                     port->port_map.local_port,
                     port->port_map.module_port,
                     port->port_map.width,
                     port->port_map.lane_bmap,
                     port->breakout_modes,
                     port->split_count,
                     port->speed_bitmap);

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_config_platform_parse(_In_ const char *platform)
{
    mlnx_platform_type_t platform_type;

    assert(platform);

    MLNX_SAI_LOG_NTC("platform: %s\n", platform);

    platform_type = (mlnx_platform_type_t)atoi(platform);

    switch (platform_type) {
    case MLNX_PLATFORM_TYPE_1710:
    case MLNX_PLATFORM_TYPE_2010:
    case MLNX_PLATFORM_TYPE_2100:
    case MLNX_PLATFORM_TYPE_2410:
    case MLNX_PLATFORM_TYPE_2420:
    case MLNX_PLATFORM_TYPE_2700:
    case MLNX_PLATFORM_TYPE_2740:
        if (!mlnx_chip_is_spc()) {
            MLNX_SAI_LOG_ERR("Failed to parse platform xml config: platform is %d (SPC1) but chip type is not SPC1\n",
                             platform_type);
            return SAI_STATUS_FAILURE;
        }
        break;


    case MLNX_PLATFORM_TYPE_3420:
    case MLNX_PLATFORM_TYPE_3700:
    case MLNX_PLATFORM_TYPE_3800:
        if (!mlnx_chip_is_spc2()) {
            MLNX_SAI_LOG_ERR("Failed to parse platform xml config: platform is %d (SPC2) but chip type is not SPC2\n",
                             platform_type);
            return SAI_STATUS_FAILURE;
        }
        break;


    case MLNX_PLATFORM_TYPE_4700:
    case MLNX_PLATFORM_TYPE_4800:
    case MLNX_PLATFORM_TYPE_4600:
    case MLNX_PLATFORM_TYPE_4600C:
        if (!mlnx_chip_is_spc3()) {
            MLNX_SAI_LOG_ERR("Failed to parse platform xml config: platform is %d (SPC3) but chip type is not SPC3\n",
                             platform_type);
            return SAI_STATUS_FAILURE;
        }
        break;

    default:
        MLNX_SAI_LOG_ERR("Unexpected platform type - %u\n", platform_type);
        return SAI_STATUS_FAILURE;
    }

    g_sai_db_ptr->platform_type = platform_type;

    return SAI_STATUS_SUCCESS;
}

static sai_status_t parse_elements(xmlDoc *doc, xmlNode * a_node)
{
    xmlNode       *cur_node, *ports_node;
    xmlChar       *key;
    sai_status_t   status;
    sx_mac_addr_t *base_mac_addr;
    const char    *profile_mac_address;

    /* parse all siblings of current element */
    for (cur_node = a_node; cur_node != NULL; cur_node = cur_node->next) {
        if ((!xmlStrcmp(cur_node->name, (const xmlChar*)"platform_info"))) {
            key = xmlGetProp(cur_node, (const xmlChar*)"type");
            if (!key) {
                MLNX_SAI_LOG_ERR("Failed to parse platform xml config: platform_info type is not specified\n");
                return SAI_STATUS_FAILURE;
            }

            status = mlnx_config_platform_parse((const char*)key);
            if (SAI_ERR(status)) {
                xmlFree(key);
                return SAI_STATUS_FAILURE;
            }
            xmlFree(key);
            return parse_elements(doc, cur_node->children);
        } else if ((!xmlStrcmp(cur_node->name, (const xmlChar*)"device-mac-address"))) {
            profile_mac_address = g_mlnx_services.profile_get_value(g_profile_id, KV_DEVICE_MAC_ADDRESS);
            if (NULL == profile_mac_address) {
                key = xmlNodeListGetString(doc, cur_node->children, 1);
                MLNX_SAI_LOG_NTC("mac: %s\n", key);
                base_mac_addr = ether_aton_r((const char*)key, &g_sai_db_ptr->base_mac_addr);
                strncpy(g_sai_db_ptr->dev_mac, (const char*)key, sizeof(g_sai_db_ptr->dev_mac));
                g_sai_db_ptr->dev_mac[sizeof(g_sai_db_ptr->dev_mac) - 1] = 0;
                xmlFree(key);
            } else {
                MLNX_SAI_LOG_NTC("mac k/v: %s\n", profile_mac_address);
                base_mac_addr = ether_aton_r(profile_mac_address, &g_sai_db_ptr->base_mac_addr);
                strncpy(g_sai_db_ptr->dev_mac, profile_mac_address, sizeof(g_sai_db_ptr->dev_mac));
                g_sai_db_ptr->dev_mac[sizeof(g_sai_db_ptr->dev_mac) - 1] = 0;
            }
            if (base_mac_addr == NULL) {
                MLNX_SAI_LOG_ERR("Error parsing device mac address\n");
                return SAI_STATUS_FAILURE;
            }
            if (base_mac_addr->ether_addr_octet[5] & (~mlnx_port_mac_mask_get())) {
                MLNX_SAI_LOG_ERR("Device mac address must be aligned by %u %02x\n",
                                 mlnx_port_mac_mask_get(), base_mac_addr->ether_addr_octet[5]);
                return SAI_STATUS_FAILURE;
            }
        } else if ((!xmlStrcmp(cur_node->name, (const xmlChar*)"number-of-physical-ports"))) {
            key                        = xmlNodeListGetString(doc, cur_node->children, 1);
            g_sai_db_ptr->ports_number = (uint32_t)atoi((const char*)key);
            MLNX_SAI_LOG_NTC("ports num: %u\n", g_sai_db_ptr->ports_number);
            xmlFree(key);
            if (g_sai_db_ptr->ports_number > MAX_PORTS) {
                MLNX_SAI_LOG_ERR("Ports number %u bigger then max %u\n", g_sai_db_ptr->ports_number, MAX_PORTS);
                return SAI_STATUS_FAILURE;
            }
        } else if ((!xmlStrcmp(cur_node->name, (const xmlChar*)"ports-list"))) {
            for (ports_node = cur_node->children; ports_node != NULL; ports_node = ports_node->next) {
                if ((!xmlStrcmp(ports_node->name, (const xmlChar*)"port-info"))) {
                    if (SAI_STATUS_SUCCESS != (status = parse_port_info(doc, ports_node->children))) {
                        return status;
                    }
                }
            }
        } else if ((!xmlStrcmp(cur_node->name, (const xmlChar*)"issu-enabled"))) {
            key                        = xmlNodeListGetString(doc, cur_node->children, 1);
            g_sai_db_ptr->issu_enabled = (uint32_t)atoi((const char*)key);
            MLNX_SAI_LOG_NTC("issu enabled: %u\n", g_sai_db_ptr->issu_enabled);
            /* divide ACL resources by half for FFB */
            g_sai_db_ptr->acl_divider = g_sai_db_ptr->issu_enabled ? 2 : 1;
            xmlFree(key);
        } else {
            /* parse all children of current element */
            if (SAI_STATUS_SUCCESS != (status = parse_elements(doc, cur_node->children))) {
                return status;
            }
        }
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_parse_config(const char *config_file)
{
    xmlDoc      *doc          = NULL;
    xmlNode     *root_element = NULL;
    sai_status_t status;

    LIBXML_TEST_VERSION;

    doc = xmlReadFile(config_file, NULL, 0);

    if (doc == NULL) {
        MLNX_SAI_LOG_ERR("could not parse config file %s\n", config_file);
        return SAI_STATUS_FAILURE;
    }

    root_element = xmlDocGetRootElement(doc);

    sai_db_write_lock();

    MLNX_SAI_LOG_NTC("Loading port map from %s ...\n", config_file);

    status = parse_elements(doc, root_element);

    if (g_sai_db_ptr->ports_configured != g_sai_db_ptr->ports_number) {
        MLNX_SAI_LOG_ERR("mismatch of port number and configuration %u %u\n",
                         g_sai_db_ptr->ports_configured, g_sai_db_ptr->ports_number);
        status = SAI_STATUS_FAILURE;
    }

    if (g_sai_db_ptr->platform_type == MLNX_PLATFORM_TYPE_INVALID) {
        MLNX_SAI_LOG_ERR("g_sai_db_ptr->platform_type (<platform_info> type in XML config) is not initialized\n");
        status = SAI_STATUS_FAILURE;
    }

    msync(g_sai_db_ptr, sizeof(*g_sai_db_ptr), MS_SYNC);
    sai_db_unlock();

    xmlFreeDoc(doc);
    xmlCleanupParser();

    return status;
}
#else /* ifndef _WIN32 */
static sai_status_t mlnx_parse_config(const char *config_file)
{
    UNUSED_PARAM(config_file);
    return SAI_STATUS_SUCCESS;
}
#endif /* ifndef _WIN32 */

static void sai_db_policer_entries_init()
{
    uint32_t ii = 0, policers_cnt = MLNX_SAI_ARRAY_LEN(g_sai_db_ptr->policers_db);

    for (ii = 0; ii < policers_cnt; ii++) {
        db_reset_policer_entry(ii);
    }
}

static void sai_db_values_init()
{
    uint32_t ii;

    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);

    memset(&g_sai_db_ptr->base_mac_addr, 0, sizeof(g_sai_db_ptr->base_mac_addr));
    memset(g_sai_db_ptr->dev_mac, 0, sizeof(g_sai_db_ptr->dev_mac));
    g_sai_db_ptr->ports_configured = 0;
    g_sai_db_ptr->ports_number     = 0;
    memset(g_sai_db_ptr->ports_db, 0, sizeof(g_sai_db_ptr->ports_db));
    memset(g_sai_db_ptr->hostif_db, 0, sizeof(g_sai_db_ptr->hostif_db));
    g_sai_db_ptr->default_trap_group = SAI_NULL_OBJECT_ID;
    g_sai_db_ptr->default_vrid       = SAI_NULL_OBJECT_ID;
    memset(&g_sai_db_ptr->callback_channel, 0, sizeof(g_sai_db_ptr->callback_channel));
    memset(g_sai_db_ptr->traps_db, 0, sizeof(g_sai_db_ptr->traps_db));
    memset(g_sai_db_ptr->qos_maps_db, 0, sizeof(g_sai_db_ptr->qos_maps_db));
    g_sai_db_ptr->qos_maps_db[MLNX_QOS_MAP_PFC_PG_INDEX].is_used    = 1;
    g_sai_db_ptr->qos_maps_db[MLNX_QOS_MAP_PFC_QUEUE_INDEX].is_used = 1;
    g_sai_db_ptr->switch_default_tc                                 = 0;
    memset(g_sai_db_ptr->policers_db, 0, sizeof(g_sai_db_ptr->policers_db));
    memset(g_sai_db_ptr->mlnx_samplepacket_session, 0, sizeof(g_sai_db_ptr->mlnx_samplepacket_session));
    memset(g_sai_db_ptr->trap_group_valid, 0, sizeof(g_sai_db_ptr->trap_group_valid));

    g_sai_db_ptr->flood_actions[MLNX_FID_FLOOD_CTRL_ATTR_UC] = SAI_PACKET_ACTION_FORWARD;
    g_sai_db_ptr->flood_actions[MLNX_FID_FLOOD_CTRL_ATTR_BC] = SAI_PACKET_ACTION_FORWARD;
    g_sai_db_ptr->flood_actions[MLNX_FID_FLOOD_CTRL_ATTR_MC] = SAI_PACKET_ACTION_FORWARD;

    g_sai_db_ptr->transaction_mode_enable = false;
    g_sai_db_ptr->issu_enabled            = false;
    g_sai_db_ptr->restart_warm            = false;
    g_sai_db_ptr->warm_recover            = false;
    g_sai_db_ptr->issu_end_called         = false;
    g_sai_db_ptr->fx_initialized          = false;
    g_sai_db_ptr->boot_type               = 0;
    g_sai_db_ptr->acl_divider             = 1;

    g_sai_db_ptr->packet_storing_mode = SX_PORT_PACKET_STORING_MODE_CUT_THROUGH;

    g_sai_db_ptr->tunnel_module_initialized         = false;
    g_sai_db_ptr->port_parsing_depth_set_for_tunnel = false;

    g_sai_db_ptr->is_bfd_module_initialized = false;

    g_sai_db_ptr->nve_tunnel_type = NVE_TUNNEL_UNKNOWN;

    g_sai_db_ptr->crc_check_enable  = true;
    g_sai_db_ptr->crc_recalc_enable = true;

    g_sai_db_ptr->platform_type = MLNX_PLATFORM_TYPE_INVALID;

    memset(g_sai_db_ptr->is_switch_priority_lossless, 0, MAX_LOSSLESS_SP * sizeof(bool));

    sai_db_policer_entries_init();

    msync(g_sai_db_ptr, sizeof(*g_sai_db_ptr), MS_SYNC);
    sai_qos_db_init();
    memset(g_sai_qos_db_ptr->wred_db, 0, sizeof(mlnx_wred_profile_t) * g_resource_limits.cos_redecn_profiles_max);
    memset(g_sai_qos_db_ptr->queue_db, 0,
           sizeof(mlnx_qos_queue_config_t) * (g_resource_limits.cos_port_ets_traffic_class_max + 1) * MAX_PORTS * 2);
    memset(g_sai_qos_db_ptr->sched_db, 0, sizeof(mlnx_sched_profile_t) * MAX_SCHED);

    for (ii = 0; ii < MAX_PORTS * 2; ii++) {
        mlnx_port_config_t *port = &g_sai_db_ptr->ports_db[ii];

        port->port_map.mapping_mode = SX_PORT_MAPPING_MODE_DISABLE;
        port->port_map.local_port   = ii + 1;
        port->index                 = ii;

        /* Set default value for ISSU LAG db */
        port->issu_lag_attr.lag_pvid = DEFAULT_VLAN;
    }

    mlnx_vlan_db_create_vlan(DEFAULT_VLAN);

    sai_qos_db_sync();
    cl_plock_release(&g_sai_db_ptr->p_lock);
}

static sai_status_t sai_db_unload(boolean_t erase_db)
{
    int          err    = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (erase_db == TRUE) {
        cl_shm_destroy(SAI_PATH);
        if (g_sai_db_ptr != NULL) {
            cl_plock_destroy(&g_sai_db_ptr->p_lock);
        }
    }

    if (g_sai_db_ptr != NULL) {
        err = munmap(g_sai_db_ptr, sizeof(*g_sai_db_ptr) + g_mlnx_shm_rm_size);
        if (err == -1) {
            SX_LOG_ERR("Failed to unmap the shared memory of the SAI DB\n");
            status = SAI_STATUS_FAILURE;
        }

        g_sai_db_ptr = NULL;
    }

    return status;
}

sai_status_t mlnx_shm_rm_rif_size_get(_Out_ size_t *size)
{
    *size = g_resource_limits.router_rifs_max;

    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_shm_rm_bridge_size_get(_Out_ size_t *size)
{
    *size = g_resource_limits.bridge_num_max;

    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_shm_rm_debug_counter_size_get(_Out_ size_t *size);
static mlnx_shm_rm_array_init_info_t mlnx_shm_array_info[MLNX_SHM_RM_ARRAY_TYPE_SIZE] = {
    [MLNX_SHM_RM_ARRAY_TYPE_RIF] = {sizeof(mlnx_rif_db_t),
                                    mlnx_shm_rm_rif_size_get,
                                    0},
    [MLNX_SHM_RM_ARRAY_TYPE_BRIDGE] = {sizeof(mlnx_bridge_t),
                                       mlnx_shm_rm_bridge_size_get,
                                       0},
    [MLNX_SHM_RM_ARRAY_TYPE_DEBUG_COUNTER] = {sizeof(mlnx_debug_counter_t),
                                              mlnx_shm_rm_debug_counter_size_get,
                                              0},
    [MLNX_SHM_RM_ARRAY_TYPE_BFD_SESSION] = {sizeof(mlnx_bfd_session_db_entry_t),
                                            NULL,
                                            MAX_BFD_SESSION_NUMBER},
};
static size_t mlnx_sai_rm_db_size_get(void)
{
    sai_status_t                   status;
    size_t                         total_size = 0, elem_count;
    mlnx_shm_rm_array_type_t       type;
    mlnx_shm_rm_array_init_info_t *init_info;

    for (type = MLNX_SHM_RM_ARRAY_TYPE_MIN; type <= MLNX_SHM_RM_ARRAY_TYPE_MAX; type++) {
        init_info = &mlnx_shm_array_info[type];
        if (!init_info->elem_count_fn && (init_info->elem_count == 0)) {
            SX_LOG_ERR("Failed to get elem count for %d - elem_count_fn == NULL\n", type);
            return 0;
        }

        if (init_info->elem_count == 0) {
            elem_count = 0;
            status     = init_info->elem_count_fn(&elem_count);
            if (SAI_ERR(status) || (elem_count == 0)) {
                SX_LOG_ERR("Failed to get elem count for %d\n", type);
                return 0;
            }

            init_info->elem_count = elem_count;
        }

        total_size += init_info->elem_count * init_info->elem_size;
    }

    return total_size;
}

static sai_status_t sai_db_create()
{
    int         err;
    int         shmid;
    cl_status_t cl_err;

    cl_err = cl_shm_create(SAI_PATH, &shmid);
    if (cl_err) {
        if (errno == EEXIST) { /* one retry is allowed */
            MLNX_SAI_LOG_ERR("Shared memory of the SAI already exists, destroying it and re-creating\n");
            cl_shm_destroy(SAI_PATH);
            cl_err = cl_shm_create(SAI_PATH, &shmid);
        }

        if (cl_err) {
            MLNX_SAI_LOG_ERR("Failed to create shared memory for SAI DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }
    }

    g_mlnx_shm_rm_size = (uint32_t)mlnx_sai_rm_db_size_get();

    if (ftruncate(shmid, sizeof(*g_sai_db_ptr) + g_mlnx_shm_rm_size) == -1) {
        MLNX_SAI_LOG_ERR("Failed to set shared memory size for the SAI DB\n");
        cl_shm_destroy(SAI_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_db_ptr =
        mmap(NULL, sizeof(*g_sai_db_ptr) + g_mlnx_shm_rm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_db_ptr == MAP_FAILED) {
        MLNX_SAI_LOG_ERR("Failed to map the shared memory of the SAI DB\n");
        g_sai_db_ptr = NULL;
        cl_shm_destroy(SAI_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    cl_err = cl_plock_init_pshared(&g_sai_db_ptr->p_lock);
    if (cl_err) {
        MLNX_SAI_LOG_ERR("Failed to initialize the SAI DB rwlock\n");
        err = munmap(g_sai_db_ptr, sizeof(*g_sai_db_ptr) + g_mlnx_shm_rm_size);
        if (err == -1) {
            MLNX_SAI_LOG_ERR("Failed to unmap the shared memory of the SAI DB\n");
        }
        g_sai_db_ptr = NULL;
        cl_shm_destroy(SAI_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    return SAI_STATUS_SUCCESS;
}

static uint8_t* mlnx_rm_offset_to_ptr(size_t rm_offset)
{
    return MLNX_SHM_RM_ARRAY_BASE_PTR + rm_offset;
}

static void* mlnx_rm_array_elem_by_idx(_In_ const mlnx_shm_rm_array_info_t *info, _In_ uint32_t idx)
{
    if (info->elem_count <= idx) {
        SX_LOG_ERR("Idx %u is out of array bounds, array size - %lu\n", idx, info->elem_count);
        return NULL;
    }

    return mlnx_rm_offset_to_ptr(info->offset_to_head) + (info->elem_size * idx);
}

static void mlnx_sai_rm_array_canary_init(mlnx_shm_rm_array_type_t type)
{
    const mlnx_shm_rm_array_info_t *info;
    mlnx_shm_array_hdr_t           *array_hdr;
    uint32_t                        ii;

    assert(MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type));

    info = &g_sai_db_ptr->array_info[type];

    for (ii = 0; ii < info->elem_count; ii++) {
        array_hdr = mlnx_rm_array_elem_by_idx(info, ii);
        assert(array_hdr);

        array_hdr->canary = MLNX_SHM_RM_ARRAY_CANARY(type);
    }
}

static sai_status_t mlnx_sai_rm_db_init(void)
{
    mlnx_shm_rm_array_type_t             type;
    const mlnx_shm_rm_array_init_info_t *init_info;
    mlnx_shm_rm_array_info_t            *info;
    uint8_t                             *shm_rm_ptr, *shm_rm_base_ptr;

    shm_rm_base_ptr = shm_rm_ptr = MLNX_SHM_RM_ARRAY_BASE_PTR;

    for (type = MLNX_SHM_RM_ARRAY_TYPE_MIN; type <= MLNX_SHM_RM_ARRAY_TYPE_MAX; type++) {
        init_info            = &mlnx_shm_array_info[type];
        info                 = &g_sai_db_ptr->array_info[type];
        info->elem_count     = init_info->elem_count;
        info->elem_size      = init_info->elem_size;
        info->offset_to_head = shm_rm_ptr - shm_rm_base_ptr;
        shm_rm_ptr          += info->elem_size * info->elem_count;

        mlnx_sai_rm_array_canary_init(type);
    }

    return SAI_STATUS_SUCCESS;
}


sai_status_t mlnx_shm_rm_idx_validate(_In_ mlnx_shm_rm_array_idx_t idx)
{
    mlnx_shm_array_hdr_t *array_hdr;

    if (!MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(idx.type)) {
        SX_LOG_ERR("Invalid idx type %d\n", idx.type);
        return SAI_STATUS_FAILURE;
    }

    array_hdr = mlnx_rm_array_elem_by_idx(&g_sai_db_ptr->array_info[idx.type], idx.idx);
    if (!array_hdr) {
        return SAI_STATUS_FAILURE;
    }

    if (!MLNX_SHM_RM_ARRAY_HDR_IS_VALID(idx.type, array_hdr)) {
        SX_LOG_ERR("array_hdr for type %d idx %d is corrupted (canary is %x, not %x)\n",
                   idx.type, idx.idx, array_hdr->canary, MLNX_SHM_RM_ARRAY_CANARY(idx.type));
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_shm_rm_array_alloc(_In_ mlnx_shm_rm_array_type_t  type,
                                     _Out_ mlnx_shm_rm_array_idx_t *idx,
                                     _Out_ void                   **elem)
{
    const mlnx_shm_rm_array_info_t *info;
    mlnx_shm_array_hdr_t           *array_hdr;
    uint32_t                        ii;

    assert(MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type));

    info = &g_sai_db_ptr->array_info[type];

    for (ii = 0; ii < info->elem_count; ii++) {
        array_hdr = mlnx_rm_array_elem_by_idx(info, ii);
        assert(array_hdr);

        if (!MLNX_SHM_RM_ARRAY_HDR_IS_VALID(type, array_hdr)) {
            SX_LOG_ERR("array_hdr for type %d idx %d is corrupted (canary is %x, not %x)\n",
                       type, ii, array_hdr->canary, MLNX_SHM_RM_ARRAY_CANARY(type));
            return SAI_STATUS_FAILURE;
        }

        if (!array_hdr->is_used) {
            array_hdr->is_used = true;
            idx->type          = type;
            idx->idx           = ii;
            *elem              = (void*)array_hdr;
            return SAI_STATUS_SUCCESS;
        }
    }

    *elem = NULL;

    return SAI_STATUS_INSUFFICIENT_RESOURCES;
}

sai_status_t mlnx_shm_rm_array_free(_In_ mlnx_shm_rm_array_idx_t idx)
{
    const mlnx_shm_rm_array_info_t *info;
    mlnx_shm_array_hdr_t           *array_hdr;
    sai_status_t                    status;

    status = mlnx_shm_rm_idx_validate(idx);
    if (SAI_ERR(status)) {
        return status;;
    }

    info = &g_sai_db_ptr->array_info[idx.type];

    array_hdr = mlnx_rm_array_elem_by_idx(info, idx.idx);
    if (!array_hdr) {
        return SAI_STATUS_FAILURE;
    }

    if (!array_hdr->is_used) {
        SX_LOG_ERR("Failed to free element - already free or not allocated\n");
        return SAI_STATUS_FAILURE;
    }

    array_hdr->is_used = false;

    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_shm_rm_array_free_entries_count(_In_ mlnx_shm_rm_array_type_t type)
{
    const mlnx_shm_rm_array_info_t *info;
    mlnx_shm_array_hdr_t           *array_hdr;
    uint32_t                        ii, free = 0;

    assert(MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type));

    info = &g_sai_db_ptr->array_info[type];

    for (ii = 0; ii < info->elem_count; ii++) {
        array_hdr = mlnx_rm_array_elem_by_idx(info, ii);
        assert(array_hdr);

        if (!MLNX_SHM_RM_ARRAY_HDR_IS_VALID(type, array_hdr)) {
            SX_LOG_ERR("array_hdr for type %d idx %d is corrupted (canary is %x, not %x)\n",
                       type, ii, array_hdr->canary, MLNX_SHM_RM_ARRAY_CANARY(type));
            return SAI_STATUS_FAILURE;
        }

        if (!array_hdr->is_used) {
            free++;
        }
    }

    return free;
}

sai_status_t mlnx_shm_rm_array_find(_In_ mlnx_shm_rm_array_type_t  type,
                                    _In_ mlnx_shm_rm_array_cmp_fn  cmp_fn,
                                    _In_ mlnx_shm_rm_array_idx_t   start_idx,
                                    _In_ const void               *data,
                                    _Out_ mlnx_shm_rm_array_idx_t *idx,
                                    _Out_ void                   **elem)
{
    const mlnx_shm_rm_array_info_t *info;
    mlnx_shm_array_hdr_t           *array_hdr;
    size_t                          array_size;
    void                           *elem_tmp;
    uint32_t                        ii, start_ii;

    assert(elem);
    assert(idx);

    if (!MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type)) {
        SX_LOG_ERR("Invalid idx type %d\n", type);
        return SAI_STATUS_FAILURE;
    }

    info       = &g_sai_db_ptr->array_info[type];
    array_size = info->elem_count;

    if ((start_idx.type == MLNX_SHM_RM_ARRAY_TYPE_INVALID) && (start_idx.idx == 0)) {
        start_ii = 0;
    } else {
        if (!MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(start_idx.type)) {
            SX_LOG_ERR("Invalid start_idx type %d\n", start_idx.type);
            return SAI_STATUS_FAILURE;
        }

        if (start_idx.type != type) {
            SX_LOG_ERR("Unexpected start_idx type %d\n", start_idx.type);
            return SAI_STATUS_FAILURE;
        }

        if (info->elem_count <= start_idx.idx) {
            SX_LOG_ERR("start_idx %u is out of array bounds, array size - %lu\n", start_idx.idx, info->elem_count);
            return SAI_STATUS_FAILURE;
        }

        start_ii = start_idx.idx;
    }

    for (ii = start_ii; ii < array_size; ii++) {
        array_hdr = mlnx_rm_array_elem_by_idx(info, ii);
        assert(array_hdr);

        if (!MLNX_SHM_RM_ARRAY_HDR_IS_VALID(type, array_hdr)) {
            SX_LOG_ERR("array_hdr for type %d idx %d is corrupted (canary is %x, not %x)\n",
                       type, ii, array_hdr->canary, MLNX_SHM_RM_ARRAY_CANARY(type));
            return SAI_STATUS_FAILURE;
        }

        if (!array_hdr->is_used) {
            continue;
        }

        elem_tmp = (void*)array_hdr;

        if (cmp_fn(elem_tmp, data)) {
            idx->type = type;
            idx->idx  = ii;
            *elem     = elem_tmp;
            return SAI_STATUS_SUCCESS;
        }
    }

    return SAI_STATUS_FAILURE;
}

sai_status_t mlnx_shm_rm_array_idx_to_ptr(_In_ mlnx_shm_rm_array_idx_t idx, _Out_ void                   **elem)
{
    mlnx_shm_array_hdr_t *array_hdr;
    sai_status_t          status;

    status = mlnx_shm_rm_idx_validate(idx);
    if (SAI_ERR(status)) {
        return status;
    }

    array_hdr = mlnx_rm_array_elem_by_idx(&g_sai_db_ptr->array_info[idx.type], idx.idx);
    if (!array_hdr) {
        return SAI_STATUS_FAILURE;
    }

    *elem = array_hdr;

    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_shm_rm_array_type_idx_to_ptr(_In_ mlnx_shm_rm_array_type_t type,
                                               _In_ uint32_t                 idx,
                                               _Out_ void                  **elem)
{
    mlnx_shm_rm_array_idx_t rm_idx;

    rm_idx.type = type;
    rm_idx.idx  = idx;

    return mlnx_shm_rm_array_idx_to_ptr(rm_idx, elem);
}

sai_status_t mlnx_shm_rm_array_type_ptr_to_idx(_In_ mlnx_shm_rm_array_type_t  type,
                                               _In_ const void               *ptr,
                                               _Out_ mlnx_shm_rm_array_idx_t *idx)
{
    const mlnx_shm_rm_array_info_t *info;
    void                           *begin, *end;

    if (!MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type)) {
        SX_LOG_ERR("Invalid type %d\n", type);
        return SAI_STATUS_FAILURE;
    }

    info = &g_sai_db_ptr->array_info[type];

    begin = mlnx_rm_offset_to_ptr(info->offset_to_head);
    end   = mlnx_rm_offset_to_ptr(info->offset_to_head) + info->elem_count * info->elem_size;

    if ((ptr < begin) || (end <= ptr)) {
        SX_LOG_ERR("ptr %p is out of range [%p, %p)\n", ptr, begin, end);
        return SAI_STATUS_FAILURE;
    }

    if (((char*)ptr - (char*)begin) % info->elem_size != 0) {
        SX_LOG_ERR("ptr %p doesn't point to the element head\n", ptr);
        return SAI_STATUS_FAILURE;
    }

    idx->type = type;
    idx->idx  = (uint32_t)(((char*)ptr - (char*)begin) / info->elem_size);

    return SAI_STATUS_SUCCESS;
}

uint32_t mlnx_shm_rm_array_size_get(_In_ mlnx_shm_rm_array_type_t type)
{
    if (!MLNX_SHM_RM_ARRAY_TYPE_IS_VALID(type)) {
        SX_LOG_ERR("Invalid idx type %d\n", type);
        return 0;
    }

    return (uint32_t)g_sai_db_ptr->array_info[type].elem_count;
}


static sai_status_t mlnx_switch_bfd_event_handle(_In_ sx_trap_id_t event,
                                                 _In_ uint64_t     opaque_data)
{
    sai_bfd_session_state_notification_t info = {0};
    mlnx_shm_rm_array_idx_t              bfd_session_db_index;
    sai_status_t                         status;

    assert(event == SX_TRAP_ID_BFD_TIMEOUT_EVENT ||
           event == SX_TRAP_ID_BFD_PACKET_EVENT);

    bfd_session_db_index = *(mlnx_shm_rm_array_idx_t*)&opaque_data;

    status = mlnx_shm_rm_idx_validate(bfd_session_db_index);
    if (SAI_ERR(status)) {
        SX_LOG_ERR("BFD DB index is invalid (opaque_data=%"PRIu64")\n", opaque_data);
        return SAI_STATUS_FAILURE;
    }

    status = mlnx_bfd_session_oid_create(bfd_session_db_index, &info.bfd_session_id);
    if (SAI_ERR(status)) {
        SX_LOG_ERR("BFD OID create failed (opaque_data=%"PRIu64")\n", opaque_data);
        return SAI_STATUS_FAILURE;
    }

    info.session_state = SAI_BFD_SESSION_STATE_DOWN;
    if (g_notification_callbacks.on_bfd_session_state_change) {
        g_notification_callbacks.on_bfd_session_state_change(1, &info);
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_dvs_mng_stage(mlnx_sai_boot_type_t boot_type, sai_object_id_t switch_id)
{
    sx_status_t           sx_status;
    sai_status_t          status;
    int                   system_err;
    char                  cmd[200];
    sx_port_attributes_t *port_attributes_p = NULL;
    uint32_t              ii;
    sx_topolib_dev_info_t dev_info;
    sx_port_mapping_t    *port_mapping = NULL;
    sx_port_log_id_t     *log_ports    = NULL;
    mlnx_port_config_t   *port;
    uint32_t              ports_to_map, nve_port_idx;
    sxd_status_t          sxd_ret     = SXD_STATUS_SUCCESS;
    const bool            is_warmboot = (BOOT_TYPE_WARM == boot_type);

    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);

    if (SX_STATUS_SUCCESS != (status = sx_api_port_swid_set(gh_sdk, SX_ACCESS_CMD_ADD, DEFAULT_ETH_SWID))) {
        SX_LOG_ERR("Port swid set failed - %s.\n", SX_STATUS_MSG(status));
        status = sdk_to_sai(status);
        goto out;
    }

    /* Set MAC address */
    snprintf(cmd, sizeof(cmd), "ip link set address %s dev swid0_eth > /dev/null 2>&1", g_sai_db_ptr->dev_mac);
    system_err = system(cmd);
    if (0 != system_err) {
        SX_LOG_ERR("Failed running \"%s\".\n", cmd);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

    /* Take swid netdev up */
    snprintf(cmd, sizeof(cmd), "ip -4 addr flush dev swid0_eth");
    system_err = system(cmd);
    if (0 != system_err) {
        SX_LOG_ERR("Failed running \"%s\".\n", cmd);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

    snprintf(cmd, sizeof(cmd), "ip link set dev swid0_eth up");
    system_err = system(cmd);
    if (0 != system_err) {
        SX_LOG_ERR("Failed running \"%s\".\n", cmd);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

    port_attributes_p = (sx_port_attributes_t*)calloc((1 + MAX_PORTS), sizeof(*port_attributes_p));
    if (NULL == port_attributes_p) {
        SX_LOG_ERR("Can't allocate port attributes\n");
        status = SAI_STATUS_NO_MEMORY;
        goto out;
    }

    port_mapping = calloc(MAX_PORTS, sizeof(*port_mapping));
    if (!port_mapping) {
        SX_LOG_ERR("Failed to allocate memory\n");
        status = SAI_STATUS_NO_MEMORY;
        goto out;
    }

    log_ports = calloc(MAX_PORTS, sizeof(*log_ports));
    if (!log_ports) {
        SX_LOG_ERR("Failed to allocate memory\n");
        status = SAI_STATUS_NO_MEMORY;
        goto out;
    }

    for (ports_to_map = 0; ports_to_map < MAX_PORTS; ports_to_map++) {
        port = mlnx_port_by_idx(ports_to_map);

        if (port->split_count > 1) {
            status = mlnx_port_auto_split(port);
            if (SAI_ERR(status)) {
                goto out;
            }
        }
    }

    for (ports_to_map = 0; ports_to_map < MAX_PORTS; ports_to_map++) {
        port = mlnx_port_by_idx(ports_to_map);

        port_attributes_p[ports_to_map].port_mode = SX_PORT_MODE_EXTERNAL;
        memcpy(&port_attributes_p[ports_to_map].port_mapping, &port->port_map, sizeof(port->port_map));
    }

    nve_port_idx = ports_to_map;

    port_attributes_p[nve_port_idx].port_mode                 = SX_PORT_MODE_NVE;
    port_attributes_p[nve_port_idx].port_mapping.mapping_mode = SX_PORT_MAPPING_MODE_DISABLE;
    ports_to_map++;

    status = sx_api_port_device_set(gh_sdk, SX_ACCESS_CMD_ADD, SX_DEVICE_ID, &g_sai_db_ptr->base_mac_addr,
                                    port_attributes_p, ports_to_map);

    if (SX_ERR(status)) {
        SX_LOG_ERR("Port device set failed - %s.\n", SX_STATUS_MSG(status));
        status = sdk_to_sai(status);
        goto out;
    }

    for (ii = 0; ii < MAX_PORTS; ii++) {
        port          = mlnx_port_by_local_id(port_attributes_p[ii].port_mapping.local_port);
        port->logical = port_attributes_p[ii].log_port;
        status        = mlnx_create_object(SAI_OBJECT_TYPE_PORT, port->logical, NULL, &port->saiport);
        if (SAI_ERR(status)) {
            goto out;
        }
    }

    g_sai_db_ptr->sx_nve_log_port = port_attributes_p[nve_port_idx].log_port;

    dev_info.dev_id          = SX_DEVICE_ID;
    dev_info.node_type       = SX_DEV_NODE_TYPE_LEAF_LOCAL;
    dev_info.unicast_arr_len = 0;
    memcpy(&dev_info.switch_mac_addr, &g_sai_db_ptr->base_mac_addr, sizeof(dev_info.switch_mac_addr));

    if (is_warmboot) {
        sxd_ret = sxd_dpt_set_access_control(SX_DEVICE_ID, READ_WRITE);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            SX_LOG_ERR("Failed to set dpt access control - %s.\n", SXD_STATUS_MSG(sxd_ret));
            status = SAI_STATUS_FAILURE;
            goto out;
        }
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_topo_device_set(gh_sdk, SX_ACCESS_CMD_ADD, &dev_info))) {
        SX_LOG_ERR("topo device add failed - %s.\n", SX_STATUS_MSG(status));
        status = sdk_to_sai(status);
        goto out;
    }

    if (is_warmboot) {
        if (SX_STATUS_SUCCESS != (status = sx_api_fdb_uc_flush_all_set(gh_sdk, DEFAULT_ETH_SWID))) {
            SX_LOG_ERR("fdb uc flush all failed - %s.\n", SX_STATUS_MSG(status));
            status = sdk_to_sai(status);
            goto out;
        }

        sxd_ret = sxd_dpt_set_access_control(SX_DEVICE_ID, READ_ONLY);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            SX_LOG_ERR("Failed to set dpt access control - %s.\n", SXD_STATUS_MSG(sxd_ret));
            status = SAI_STATUS_FAILURE;
            goto out;
        }
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_topo_device_set(gh_sdk, SX_ACCESS_CMD_READY, &dev_info))) {
        SX_LOG_ERR("topo device set ready failed - %s.\n", SX_STATUS_MSG(status));
        status = sdk_to_sai(status);
        goto out;
    }

    if (g_notification_callbacks.on_switch_state_change) {
        g_notification_callbacks.on_switch_state_change(switch_id, SAI_SWITCH_OPER_STATUS_UP);
    }

    ports_to_map = 0;
    for (ii = 0; ii < MAX_PORTS; ii++) {
        if (!g_sai_db_ptr->ports_db[ii].is_present) {
            continue;
        }

        port_mapping[ports_to_map].mapping_mode = SX_PORT_MAPPING_MODE_DISABLE;
        port_mapping[ports_to_map].local_port   = g_sai_db_ptr->ports_db[ii].port_map.local_port;
        log_ports[ports_to_map]                 = g_sai_db_ptr->ports_db[ii].logical;
        ports_to_map++;
    }

    if (!is_warmboot) {
        sx_status = sx_api_port_mapping_set(gh_sdk, log_ports, port_mapping, ports_to_map);
        if (SX_ERR(sx_status)) {
            SX_LOG_ERR("Failed to unmap ports - %s\n", SX_STATUS_MSG(sx_status));
            status = sdk_to_sai(sx_status);
            goto out;
        }
    }

    ports_to_map = 0;
    for (ii = 0; ii < MAX_PORTS; ii++) {
        if (!g_sai_db_ptr->ports_db[ii].is_present) {
            continue;
        }

        port_mapping[ports_to_map] = g_sai_db_ptr->ports_db[ii].port_map;
        ports_to_map++;
    }

    if (!is_warmboot) {
        sx_status = sx_api_port_mapping_set(gh_sdk, log_ports, port_mapping, ports_to_map);
        if (SX_ERR(sx_status)) {
            SX_LOG_ERR("Failed to map ports - %s\n", SX_STATUS_MSG(sx_status));
            status = sdk_to_sai(sx_status);
            goto out;
        }
    }

    if (is_warmboot) {
        sxd_ret = sxd_dpt_set_access_control(SX_DEVICE_ID, READ_WRITE);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            SX_LOG_ERR("Failed to set dpt access control - %s.\n", SXD_STATUS_MSG(sxd_ret));
            status = SAI_STATUS_FAILURE;
            goto out;
        }
    }

    status = mlnx_stp_preinitialize();
    if (SAI_ERR(status)) {
        goto out;
    }

    /* Need to read SDK port list twice:
     * First time: SDK port list only contains all physical ports
     * Do sx_api_port_init_set to initialize SDK ports, SDK will update LAG information
     * Second time: SDK port list contains all physical ports and LAGs */
    if (is_warmboot) {
        status = mlnx_update_issu_port_db();
        if (SAI_ERR(status)) {
            SX_LOG_ERR("Failed to get SDK LAG\n");
        }
    }

    for (ii = 0; ii < MAX_PORTS; ii++) {
        port = &mlnx_ports_db[ii];
        if (port->logical &&
            ((is_warmboot && port->sdk_port_added) ||
             (!is_warmboot && port->is_present))) {
            status = mlnx_port_config_init_mandatory(port);
            if (SAI_ERR(status)) {
                SX_LOG_ERR("Failed initialize port oid %" PRIx64 " config\n", port->saiport);
                goto out;
            }
        }
    }

    if (is_warmboot) {
        status = mlnx_update_issu_port_db();
        if (SAI_ERR(status)) {
            SX_LOG_ERR("Failed to get SDK LAG\n");
        }
    }

    mlnx_port_phy_foreach(port, ii) {
        if (!is_warmboot || (port->is_present && port->sdk_port_added)) {
            status = mlnx_port_config_init(port);
            if (SAI_ERR(status)) {
                SX_LOG_ERR("Failed initialize port oid %" PRIx64 " config\n", port->saiport);
                goto out;
            }

            if (!is_warmboot) {
                status = mlnx_port_speed_bitmap_apply(port);
                if (SAI_ERR(status)) {
                    goto out;
                }
            }

            if ((!is_warmboot) && (mlnx_chip_is_spc2())) {
                status = mlnx_port_fec_set_impl(port->logical, SAI_PORT_FEC_MODE_NONE);
                if (SAI_ERR(status)) {
                    goto out;
                }
            }
        }
    }

    if (is_warmboot) {
        sxd_ret = sxd_dpt_set_access_control(SX_DEVICE_ID, READ_WRITE);
        if (SXD_CHECK_FAIL(sxd_ret)) {
            SX_LOG_ERR("Failed to set dpt access control - %s.\n", SXD_STATUS_MSG(sxd_ret));
            status = SAI_STATUS_FAILURE;
            goto out;
        }
    }

    if (SAI_STATUS_SUCCESS != (status = mlnx_wred_init())) {
        goto out;
    }

out:
    sai_db_unlock();
    if (NULL != port_attributes_p) {
        free(port_attributes_p);
    }
    free(port_mapping);
    free(log_ports);

    return status;
}

static sai_status_t mlnx_switch_fdb_record_age(_In_ const sx_fdb_notify_record_t *fdb_record)
{
    sx_status_t                 sx_status;
    sx_fdb_uc_mac_addr_params_t mac_entry;
    uint32_t                    entries_count;

    assert(fdb_record);

    memset(&mac_entry, 0, sizeof(mac_entry));

    mac_entry.fid_vid    = fdb_record->fid;
    mac_entry.log_port   = fdb_record->log_port;
    mac_entry.entry_type = SX_FDB_UC_AGEABLE;
    /* Dynamic MAC entries are limited to FORWARD action only */
    mac_entry.action = SX_FDB_ACTION_FORWARD;

    memcpy(&mac_entry.mac_addr, &fdb_record->mac_addr, sizeof(mac_entry.mac_addr));

    entries_count = 1;

    sx_status = sx_api_fdb_uc_mac_addr_set(gh_sdk, SX_ACCESS_CMD_DELETE, DEFAULT_ETH_SWID, &mac_entry, &entries_count);
    if (SX_ERR(sx_status)) {
        SX_LOG_ERR("Failed to age fdb entry - %s.\n", SX_STATUS_MSG(sx_status));
        return sdk_to_sai(sx_status);
    }

    if (entries_count != 1) {
        SX_LOG_ERR("Failed to age fdb entry - aged count is - %d", entries_count);
        return SAI_STATUS_FAILURE;
    }

    return SAI_STATUS_SUCCESS;
}

/*
 * FDB entry needs to be aged if at least one of the learning modes from port/fid/global is CONTROL_LEARN
 * Currently SAI allows setting control mode only on port, fid/global are only auto or disabled, so
 * checking only port mode.
 * SX_FDB_LEARN_MODE_DONT_LEARN is not checked since there should be no events
 */
static sai_status_t mlnx_switch_fdb_record_check_and_age(_In_ const sx_fdb_notify_record_t *fdb_record)
{
    sai_status_t        status;
    sx_status_t         sx_status;
    sx_port_log_id_t    sx_port_id;
    sx_fdb_learn_mode_t sx_fdb_learn_mode;

    assert(fdb_record);

    if ((fdb_record->type != SX_FDB_NOTIFY_TYPE_AGED_MAC_LAG) &&
        (fdb_record->type != SX_FDB_NOTIFY_TYPE_AGED_MAC_PORT)) {
        return SAI_STATUS_SUCCESS;
    }

    sx_port_id = fdb_record->log_port;

    sx_status = sx_api_fdb_port_learn_mode_get(gh_sdk, sx_port_id, &sx_fdb_learn_mode);
    if (SX_ERR(sx_status)) {
        SX_LOG_ERR("Failed to get port %x learn mode - %s\n", sx_port_id, SX_STATUS_MSG(sx_status));
        return sdk_to_sai(sx_status);
    }

    SX_LOG_DBG("Port %x learn mode is %s\n", sx_port_id, SX_LEARN_MODE_MSG(sx_fdb_learn_mode));

    if (sx_fdb_learn_mode == SX_FDB_LEARN_MODE_CONTROL_LEARN) {
        status = mlnx_switch_fdb_record_age(fdb_record);
        if (SAI_ERR(status)) {
            return status;
        }

        return SAI_STATUS_SUCCESS;
    }

    SX_LOG_DBG("No need to age - learn mode is auto\n");

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_parse_fdb_event(uint8_t                           *p_packet,
                                                sx_receive_info_t                 *receive_info,
                                                sai_fdb_event_notification_data_t *fdb_events,
                                                uint32_t                          *event_count,
                                                sai_attribute_t                   *attr_list)
{
    uint32_t                    from_index, to_index = 0;
    sx_fdb_notify_data_t       *packet = (sx_fdb_notify_data_t*)p_packet;
    sx_fid_t                    sx_fid;
    sai_attribute_t            *attr_ptr = attr_list;
    sai_status_t                status   = SAI_STATUS_SUCCESS;
    sx_fdb_uc_mac_addr_params_t mac_entry;
    sai_object_id_t             port_id = SAI_NULL_OBJECT_ID;
    sai_mac_t                   mac_addr;
    sai_object_id_t             switch_id;
    mlnx_object_id_t            mlnx_switch_id = { 0 };
    bool                        has_port;

    /* hard coded single switch instance */
    mlnx_switch_id.id.is_created = true;
    mlnx_object_id_to_sai(SAI_OBJECT_TYPE_SWITCH, &mlnx_switch_id, &switch_id);

    for (from_index = 0; from_index < packet->records_num; from_index++) {
        SX_LOG_INF(
            "FDB event received [%u/%u, %u] vlan: %4u ; mac: %02x:%02x:%02x:%02x:%02x:%02x ; log_port: (0x%08X) ; type: %s(%d)\n",
            from_index + 1,
            packet->records_num,
            to_index + 1,
            packet->records_arr[from_index].fid,
            packet->records_arr[from_index].mac_addr.ether_addr_octet[0],
            packet->records_arr[from_index].mac_addr.ether_addr_octet[1],
            packet->records_arr[from_index].mac_addr.ether_addr_octet[2],
            packet->records_arr[from_index].mac_addr.ether_addr_octet[3],
            packet->records_arr[from_index].mac_addr.ether_addr_octet[4],
            packet->records_arr[from_index].mac_addr.ether_addr_octet[5],
            packet->records_arr[from_index].log_port,
            SX_FDB_NOTIFY_TYPE_STR(packet->records_arr[from_index].type),
            packet->records_arr[from_index].type);

        port_id = SAI_NULL_OBJECT_ID;
        sx_fid  = 0;
        memset(mac_addr, 0, sizeof(mac_addr));
        memset(&fdb_events[to_index], 0, sizeof(fdb_events[to_index]));
        memset(&mac_entry, 0, sizeof(mac_entry));
        has_port = false;

        mlnx_switch_fdb_record_check_and_age(&packet->records_arr[from_index]);

        switch (packet->records_arr[from_index].type) {
        case SX_FDB_NOTIFY_TYPE_NEW_MAC_LAG:
        case SX_FDB_NOTIFY_TYPE_NEW_MAC_PORT:
            fdb_events[to_index].event_type = SAI_FDB_EVENT_LEARNED;
            memcpy(&mac_addr, packet->records_arr[from_index].mac_addr.ether_addr_octet, sizeof(mac_addr));
            sx_fid   = packet->records_arr[from_index].fid;
            has_port = true;
            break;

        case SX_FDB_NOTIFY_TYPE_AGED_MAC_LAG:
        case SX_FDB_NOTIFY_TYPE_AGED_MAC_PORT:
            fdb_events[to_index].event_type = SAI_FDB_EVENT_AGED;
            memcpy(&mac_addr, packet->records_arr[from_index].mac_addr.ether_addr_octet, sizeof(mac_addr));
            sx_fid   = packet->records_arr[from_index].fid;
            has_port = true;
            break;

        case SX_FDB_NOTIFY_TYPE_FLUSH_ALL:
            fdb_events[to_index].event_type = SAI_FDB_EVENT_FLUSHED;
            break;

        case SX_FDB_NOTIFY_TYPE_FLUSH_LAG:
        case SX_FDB_NOTIFY_TYPE_FLUSH_PORT:
            fdb_events[to_index].event_type = SAI_FDB_EVENT_FLUSHED;
            has_port                        = true;
            break;

        case SX_FDB_NOTIFY_TYPE_FLUSH_PORT_FID:
        case SX_FDB_NOTIFY_TYPE_FLUSH_LAG_FID:
            has_port = true;
            /* Falls through. */

        case SX_FDB_NOTIFY_TYPE_FLUSH_FID:
            fdb_events[to_index].event_type = SAI_FDB_EVENT_FLUSHED;
            sx_fid                          = packet->records_arr[from_index].fid;
            break;

        default:
            SX_LOG_ERR("Unexpected record [%u/%u, %u] type %d %s\n",
                       from_index + 1, packet->records_num, to_index + 1,
                       packet->records_arr[from_index].type,
                       SX_FDB_NOTIFY_TYPE_STR(packet->records_arr[from_index].type));
            continue;
        }

        memcpy(&fdb_events[to_index].fdb_entry.mac_address, mac_addr,
               sizeof(fdb_events[to_index].fdb_entry.mac_address));

        if (has_port) {
            /*
             * In some cases, FDB event is generated for the port that is not on the bridge
             * e.g. when the port is added to the LAG
             * In this case we don't need to print en error message for user
             */
            status = mlnx_log_port_to_sai_bridge_port_soft(packet->records_arr[from_index].log_port, &port_id);
            if (SAI_ERR(status)) {
                SX_LOG_INF("FDB event [%u/%u, %u] without matching bridge port %x\n",
                           from_index + 1, packet->records_num, to_index + 1,
                           packet->records_arr[from_index].log_port);
                continue;
            }
        }

        if (sx_fid > 0) {
            if (packet->records_arr[from_index].fid < MIN_SX_BRIDGE_ID) {
                status = mlnx_vlan_oid_create(sx_fid, &fdb_events[to_index].fdb_entry.bv_id);
                if (SAI_ERR(status)) {
                    SX_LOG_ERR("Failed to convert sx fid to bv_id [%u/%u, %u]\n",
                               from_index + 1, packet->records_num, to_index + 1);
                    continue;
                }
            } else {
                status = mlnx_create_bridge_1d_object(sx_fid, &fdb_events[to_index].fdb_entry.bv_id);
                if (SAI_ERR(status)) {
                    SX_LOG_ERR("Failed to convert sx fid to bv_id [%u/%u, %u]\n",
                               from_index + 1, packet->records_num, to_index + 1);
                    continue;
                }
            }
        } else {
            fdb_events[to_index].fdb_entry.bv_id = SAI_NULL_OBJECT_ID;
        }

        fdb_events[to_index].fdb_entry.switch_id = switch_id;

        fdb_events[to_index].attr       = attr_ptr;
        fdb_events[to_index].attr_count = FDB_NOTIF_ATTRIBS_NUM;

        attr_ptr->id        = SAI_FDB_ENTRY_ATTR_BRIDGE_PORT_ID;
        attr_ptr->value.oid = port_id;
        ++attr_ptr;

        attr_ptr->id        = SAI_FDB_ENTRY_ATTR_TYPE;
        attr_ptr->value.s32 = SAI_FDB_ENTRY_TYPE_DYNAMIC;
        ++attr_ptr;

        attr_ptr->id        = SAI_FDB_ENTRY_ATTR_PACKET_ACTION;
        attr_ptr->value.s32 = SAI_PACKET_ACTION_FORWARD;
        ++attr_ptr;

        ++to_index;
    }
    *event_count = to_index;
    return SAI_STATUS_SUCCESS;
}

static void event_thread_func(void *context)
{
#define MAX_PACKET_SIZE MAX(g_resource_limits.port_mtu_max, SX_HOST_EVENT_BUFFER_SIZE_MAX)

    sx_status_t                         status;
    sx_api_handle_t                     api_handle;
    sx_user_channel_t                   port_channel, callback_channel;
    fd_set                              descr_set;
    int                                 ret_val;
    sai_object_id_t                     switch_id = (sai_object_id_t)context;
    uint8_t                            *p_packet  = NULL;
    uint32_t                            packet_size;
    sx_receive_info_t                  *receive_info = NULL;
    sai_port_oper_status_notification_t port_data;
    struct timeval                      timeout;
    sai_attribute_t                     callback_data[RECV_ATTRIBS_NUM];
    sai_hostif_trap_type_t              trap_id;
    const char                         *trap_name;
    mlnx_trap_type_t                    trap_type;
    sai_fdb_event_notification_data_t  *fdb_events             = NULL;
    sai_attribute_t                    *attr_list              = NULL;
    uint32_t                            event_count            = 0;
    bool                                is_warmboot_init_stage = false;
#ifdef ACS_OS
    bool           transaction_mode_enable = false;
    const uint8_t  fastboot_wait_time      = 180;
    struct timeval time_init;
    struct timeval time_now;
    bool           stop_timer = false;
#endif

    memset(&port_channel, 0, sizeof(port_channel));
    memset(&callback_channel, 0, sizeof(callback_channel));

#ifdef ACS_OS
    gettimeofday(&time_init, NULL);
#endif

    callback_data[0].id = SAI_HOSTIF_PACKET_ATTR_HOSTIF_TRAP_ID;
    callback_data[1].id = SAI_HOSTIF_PACKET_ATTR_INGRESS_PORT;
    callback_data[2].id = SAI_HOSTIF_PACKET_ATTR_INGRESS_LAG;

    if (SX_STATUS_SUCCESS != (status = sx_api_open(sai_log_cb, &api_handle))) {
        MLNX_SAI_LOG_ERR("Can't open connection to SDK - %s.\n", SX_STATUS_MSG(status));
        if (g_notification_callbacks.on_switch_shutdown_request) {
            g_notification_callbacks.on_switch_shutdown_request(switch_id);
        }
        return;
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_open(api_handle, &port_channel.channel.fd))) {
        SX_LOG_ERR("host ifc open port fd failed - %s.\n", SX_STATUS_MSG(status));
        goto out;
    }

    receive_info = (sx_receive_info_t*)calloc(1, sizeof(*receive_info));
    if (NULL == receive_info) {
        SX_LOG_ERR("Can't allocate receive_info memory\n");
        status = SX_STATUS_NO_MEMORY;
        goto out;
    }

    p_packet = (uint8_t*)malloc(sizeof(*p_packet) * MAX_PACKET_SIZE);
    if (NULL == p_packet) {
        SX_LOG_ERR("Can't allocate packet memory\n");
        status = SX_STATUS_ERROR;
        goto out;
    }
    SX_LOG_NTC("Event packet buffer size %u\n", MAX_PACKET_SIZE);

    fdb_events = calloc(SX_FDB_NOTIFY_SIZE_MAX, sizeof(sai_fdb_event_notification_data_t));
    if (NULL == fdb_events) {
        SX_LOG_ERR("Can't allocate memory for fdb events\n");
        status = SX_STATUS_ERROR;
        goto out;
    }

    attr_list = calloc(SX_FDB_NOTIFY_SIZE_MAX * FDB_NOTIF_ATTRIBS_NUM, sizeof(sai_attribute_t));
    if (NULL == attr_list) {
        SX_LOG_ERR("Can't allocate memory for attribute list\n");
        status = SX_STATUS_ERROR;
        goto out;
    }

    port_channel.type = SX_USER_CHANNEL_TYPE_FD;
    if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_trap_id_register_set(api_handle, SX_ACCESS_CMD_REGISTER,
                                                                            DEFAULT_ETH_SWID, SX_TRAP_ID_PUDE,
                                                                            &port_channel))) {
        SX_LOG_ERR("host ifc trap register PUDE failed - %s.\n", SX_STATUS_MSG(status));
        goto out;
    }

    cl_plock_acquire(&g_sai_db_ptr->p_lock);
    memcpy(&callback_channel, &g_sai_db_ptr->callback_channel, sizeof(callback_channel));
    cl_plock_release(&g_sai_db_ptr->p_lock);

    {
        if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_trap_id_register_set(api_handle, SX_ACCESS_CMD_REGISTER,
                                                                                DEFAULT_ETH_SWID,
                                                                                SX_TRAP_ID_BFD_TIMEOUT_EVENT,
                                                                                &callback_channel))) {
            SX_LOG_ERR("host ifc trap register SX_TRAP_ID_BFD_TIMEOUT_EVENT failed - %s.\n", SX_STATUS_MSG(status));
            goto out;
        }

        if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_trap_id_register_set(api_handle, SX_ACCESS_CMD_REGISTER,
                                                                                DEFAULT_ETH_SWID,
                                                                                SX_TRAP_ID_BFD_PACKET_EVENT,
                                                                                &callback_channel))) {
            SX_LOG_ERR("host ifc trap register SX_TRAP_ID_BFD_PACKET_EVENT failed - %s.\n", SX_STATUS_MSG(status));
            goto out;
        }
    }


    while (!event_thread_asked_to_stop) {
        FD_ZERO(&descr_set);
        FD_SET(port_channel.channel.fd.fd, &descr_set);
        FD_SET(callback_channel.channel.fd.fd, &descr_set);

        timeout.tv_sec  = 1;
        timeout.tv_usec = 0;

#ifdef ACS_OS
        if (!stop_timer) {
            gettimeofday(&time_now, NULL);
            if (fastboot_wait_time <= (time_now.tv_sec - time_init.tv_sec)) {
                stop_timer = true;
                cl_plock_acquire(&g_sai_db_ptr->p_lock);
                transaction_mode_enable = g_sai_db_ptr->transaction_mode_enable;
                if (transaction_mode_enable) {
                    if (SX_STATUS_SUCCESS !=
                        (status = sx_api_transaction_mode_set(gh_sdk, SX_ACCESS_CMD_DISABLE))) {
                        MLNX_SAI_LOG_ERR("Failed to set transaction mode to disable: %s\n",
                                         SX_STATUS_MSG(status));
                        cl_plock_release(&g_sai_db_ptr->p_lock);
                        goto out;
                    }
                    MLNX_SAI_LOG_DBG("Disabled transaction mode\n");
                    g_sai_db_ptr->transaction_mode_enable = false;
                }
                cl_plock_release(&g_sai_db_ptr->p_lock);
            }
        }
#endif

        ret_val = select(FD_SETSIZE, &descr_set, NULL, NULL, &timeout);

        if (-1 == ret_val) {
            SX_LOG_ERR("select ended with error/interrupt %s\n", strerror(errno));
            status = SX_STATUS_ERROR;
            goto out;
        }

        if (ret_val > 0) {
            if (FD_ISSET(port_channel.channel.fd.fd, &descr_set)) {
                packet_size = MAX_PACKET_SIZE;
                if (SX_STATUS_SUCCESS !=
                    (status = sx_lib_host_ifc_recv(&port_channel.channel.fd, p_packet, &packet_size, receive_info))) {
                    SX_LOG_ERR("sx_api_host_ifc_recv on port fd failed with error %s out size %u\n",
                               SX_STATUS_MSG(status), packet_size);
                    goto out;
                }

                if (SX_INVALID_PORT == receive_info->source_log_port) {
                    SX_LOG_WRN("sx_api_host_ifc_recv on port fd returned unknown port, waiting for next packet\n");
                    continue;
                }

                if (SAI_STATUS_SUCCESS !=
                    (status = mlnx_create_object(SAI_OBJECT_TYPE_PORT, receive_info->event_info.pude.log_port, NULL,
                                                 &port_data.port_id))) {
                    goto out;
                }

                if (SX_PORT_OPER_STATUS_UP == receive_info->event_info.pude.oper_state) {
                    port_data.port_state = SAI_PORT_OPER_STATUS_UP;
                } else {
                    port_data.port_state = SAI_PORT_OPER_STATUS_DOWN;
                }
                SX_LOG_NTC("Port %x changed state to %s\n", receive_info->event_info.pude.log_port,
                           (SX_PORT_OPER_STATUS_UP == receive_info->event_info.pude.oper_state) ? "up" : "down");

                if (g_notification_callbacks.on_port_state_change) {
                    g_notification_callbacks.on_port_state_change(1, &port_data);
                }
            }

            if (FD_ISSET(callback_channel.channel.fd.fd, &descr_set)) {
                packet_size = MAX_PACKET_SIZE;
                if (SX_STATUS_SUCCESS !=
                    (status =
                         sx_lib_host_ifc_recv(&callback_channel.channel.fd, p_packet, &packet_size, receive_info))) {
                    SX_LOG_ERR("sx_api_host_ifc_recv on callback fd failed with error %s out size %u\n",
                               SX_STATUS_MSG(status), packet_size);
                    goto out;
                }

                if (SX_TRAP_ID_BFD_PACKET_EVENT == receive_info->trap_id) {
                    const struct bfd_packet_event *event = (const struct bfd_packet_event*)p_packet;
                    if (event->opaque_data_valid) {
                        status = mlnx_switch_bfd_event_handle(SX_TRAP_ID_BFD_PACKET_EVENT,
                                                              event->opaque_data);
                        if (SAI_ERR(status)) {
                            SX_LOG_ERR("BFD event handle failed.\n");
                        }
                    } else {
                        SX_LOG_WRN("Received BFD packet not related to any existing session.\n");
                    }
                    continue;
                }

                if (SX_TRAP_ID_BFD_TIMEOUT_EVENT == receive_info->trap_id) {
                    status = mlnx_switch_bfd_event_handle(SX_TRAP_ID_BFD_TIMEOUT_EVENT,
                                                          receive_info->event_info.bfd_timeout.timeout_event.opaque_data);
                    if (SAI_ERR(status)) {
                        SX_LOG_ERR("BFD event handle failed.\n");
                    }
                    continue;
                }


                if (SAI_STATUS_SUCCESS !=
                    (status =
                         mlnx_translate_sdk_trap_to_sai(receive_info->trap_id, &trap_id, &trap_name, &trap_type))) {
                    SX_LOG_WRN("unknown sdk trap %u, waiting for next packet\n", receive_info->trap_id);
                    continue;
                }

                if (SX_TRAP_ID_FDB_EVENT == receive_info->trap_id) {
                    SX_LOG_INF("Received trap %s sdk %u\n", trap_name, receive_info->trap_id);

                    if (SAI_STATUS_SUCCESS != (status = mlnx_switch_parse_fdb_event(p_packet, receive_info,
                                                                                    fdb_events, &event_count,
                                                                                    attr_list))) {
                        continue;
                    }

                    /* event arrived from sdk, but had only records with no matching bridge port */
                    if (0 == event_count) {
                        continue;
                    }

                    if (g_notification_callbacks.on_fdb_event) {
                        g_notification_callbacks.on_fdb_event(event_count, fdb_events);
                    }

                    continue;
                }

                if (SX_INVALID_PORT == receive_info->source_log_port) {
                    SX_LOG_WRN("sx_api_host_ifc_recv on callback fd returned unknown port, waiting for next packet\n");
                    continue;
                }

                if (SAI_STATUS_SUCCESS !=
                    (status =
                         mlnx_create_object((trap_type ==
                                             MLNX_TRAP_TYPE_REGULAR) ? SAI_OBJECT_TYPE_HOSTIF_TRAP :
                                            SAI_OBJECT_TYPE_HOSTIF_USER_DEFINED_TRAP,
                                            trap_id, NULL, &callback_data[0].value.oid))) {
                    goto out;
                }

                if (SAI_STATUS_SUCCESS !=
                    (status = mlnx_create_object(SAI_OBJECT_TYPE_PORT, receive_info->source_log_port, NULL,
                                                 &callback_data[1].value.oid))) {
                    goto out;
                }

                if (receive_info->is_lag) {
                    cl_plock_acquire(&g_sai_db_ptr->p_lock);
                    is_warmboot_init_stage = (BOOT_TYPE_WARM == g_sai_db_ptr->boot_type) &&
                                             (!g_sai_db_ptr->issu_end_called);
                    status = mlnx_log_port_to_object(receive_info->source_lag_port,
                                                     &callback_data[2].value.oid);
                    cl_plock_release(&g_sai_db_ptr->p_lock);
                    if (SAI_ERR(status)) {
                        if (!is_warmboot_init_stage) {
                            goto out;
                        } else {
                            /* During ISSU, LAG is not created yet, or LAG member is not added yet */
                            callback_data[2].value.oid = SAI_NULL_OBJECT_ID;
                        }
                    }
                } else {
                    callback_data[2].value.oid = SAI_NULL_OBJECT_ID;
                }

                SX_LOG_INF("Received trap %s sdk %u port %x is lag %u %x\n", trap_name, receive_info->trap_id,
                           receive_info->source_log_port, receive_info->is_lag, receive_info->source_lag_port);

                if (g_notification_callbacks.on_packet_event) {
                    g_notification_callbacks.on_packet_event(switch_id,
                                                             packet_size,
                                                             p_packet,
                                                             RECV_ATTRIBS_NUM,
                                                             callback_data);
                }
            }
        }
    }

out:
    SX_LOG_NTC("Closing event thread - %s.\n", SX_STATUS_MSG(status));

    if (SX_STATUS_SUCCESS != status) {
        if (g_notification_callbacks.on_switch_shutdown_request) {
            g_notification_callbacks.on_switch_shutdown_request(switch_id);
        }
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_close(api_handle, &port_channel.channel.fd))) {
        SX_LOG_ERR("host ifc close port fd failed - %s.\n", SX_STATUS_MSG(status));
    }

    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);
    if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_close(api_handle, &callback_channel.channel.fd))) {
        SX_LOG_ERR("host ifc close callback fd failed - %s.\n", SX_STATUS_MSG(status));
    }
    memset(&g_sai_db_ptr->callback_channel, 0, sizeof(g_sai_db_ptr->callback_channel));
    msync(g_sai_db_ptr, sizeof(*g_sai_db_ptr), MS_SYNC);
    cl_plock_release(&g_sai_db_ptr->p_lock);

    if (NULL != p_packet) {
        free(p_packet);
    }

    if (NULL != fdb_events) {
        free(fdb_events);
    }

    if (NULL != attr_list) {
        free(attr_list);
    }

    free(receive_info);

    if (SX_STATUS_SUCCESS != (status = sx_api_close(&api_handle))) {
        SX_LOG_ERR("API close failed.\n");
    }
}

/* Arrange memory for QoS DB.
 *  *wred_db
 *  *sched_db
 *  array for all wred profiles
 *  array of port qos config
 *  array of all queues for all ports
 */
static void sai_qos_db_init()
{
    g_sai_qos_db_ptr->wred_db = (mlnx_wred_profile_t*)(((uint8_t*)g_sai_qos_db_ptr->db_base_ptr));

    g_sai_qos_db_ptr->sched_db = (mlnx_sched_profile_t*)((uint8_t*)g_sai_qos_db_ptr->wred_db +
                                                         (sizeof(mlnx_wred_profile_t) *
                                                          g_resource_limits.cos_redecn_profiles_max));

    g_sai_qos_db_ptr->queue_db = (mlnx_qos_queue_config_t*)((uint8_t*)g_sai_qos_db_ptr->sched_db +
                                                            sizeof(mlnx_sched_profile_t) * MAX_SCHED);
}

static sai_status_t sai_qos_db_unload(boolean_t erase_db)
{
    int          err    = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (erase_db == TRUE) {
        cl_shm_destroy(SAI_QOS_PATH);
    }

    if (g_sai_qos_db_ptr == NULL) {
        return status;
    }

    if (g_sai_qos_db_ptr->db_base_ptr != NULL) {
        err = munmap(g_sai_qos_db_ptr->db_base_ptr, g_sai_qos_db_size);
        if (err == -1) {
            SX_LOG_ERR("Failed to unmap the shared memory of the SAI QOS DB\n");
            status = SAI_STATUS_FAILURE;
        }
    }

    free(g_sai_qos_db_ptr);
    g_sai_qos_db_ptr = NULL;

    return status;
}

static uint32_t sai_qos_db_size_get()
{
    return ((sizeof(mlnx_wred_profile_t) * g_resource_limits.cos_redecn_profiles_max) +
            (((sizeof(mlnx_qos_queue_config_t) *
               (g_resource_limits.cos_port_ets_traffic_class_max + 1))) * MAX_PORTS * 2) +
            sizeof(mlnx_sched_profile_t) * MAX_SCHED);
}

/* g_resource_limits must be initialized before we call create,
 * we need it to calculate size of shared memory */
static sai_status_t sai_qos_db_create()
{
    int         shmid;
    cl_status_t cl_err;

    cl_err = cl_shm_create(SAI_QOS_PATH, &shmid);
    if (cl_err) {
        if (errno == EEXIST) { /* one retry is allowed */
            MLNX_SAI_LOG_WRN("Shared memory of the SAI QOS already exists, destroying it and re-creating\n");
            cl_shm_destroy(SAI_QOS_PATH);
            cl_err = cl_shm_create(SAI_QOS_PATH, &shmid);
        }

        if (cl_err) {
            MLNX_SAI_LOG_ERR("Failed to create shared memory for SAI QOS DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }
    }

    g_sai_qos_db_size = sai_qos_db_size_get();

    if (ftruncate(shmid, g_sai_qos_db_size) == -1) {
        MLNX_SAI_LOG_ERR("Failed to set shared memory size for the SAI QOS DB\n");
        cl_shm_destroy(SAI_QOS_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_qos_db_ptr = malloc(sizeof(*g_sai_qos_db_ptr));
    if (g_sai_qos_db_ptr == NULL) {
        MLNX_SAI_LOG_ERR("Failed to allocate SAI QoS DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_qos_db_ptr->db_base_ptr = mmap(NULL, g_sai_qos_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_qos_db_ptr->db_base_ptr == MAP_FAILED) {
        MLNX_SAI_LOG_ERR("Failed to map the shared memory of the SAI QOS DB\n");
        g_sai_qos_db_ptr->db_base_ptr = NULL;
        cl_shm_destroy(SAI_QOS_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    return SAI_STATUS_SUCCESS;
}

/* NOTE:  g_sai_db_ptr->ports_number must be initializes before calling this method*/
static void sai_buffer_db_values_init()
{
    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);
    assert(g_sai_db_ptr->ports_number != 0);
    sai_buffer_db_pointers_init();
    sai_buffer_db_data_reset();
    cl_plock_release(&g_sai_db_ptr->p_lock);
}

static sai_status_t sai_buffer_db_switch_connect_init(int shmid)
{
    init_buffer_resource_limits();
    g_sai_buffer_db_size = sai_buffer_db_size_get();
    g_sai_buffer_db_ptr  = malloc(sizeof(*g_sai_buffer_db_ptr));
    if (g_sai_buffer_db_ptr == NULL) {
        SX_LOG_ERR("Failed to allocate SAI buffer DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_buffer_db_ptr->db_base_ptr = mmap(NULL, g_sai_buffer_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_buffer_db_ptr->db_base_ptr == MAP_FAILED) {
        SX_LOG_ERR("Failed to map the shared memory of the SAI buffer DB\n");
        g_sai_buffer_db_ptr->db_base_ptr = NULL;
        return SAI_STATUS_NO_MEMORY;
    }
    sai_buffer_db_pointers_init();
    msync(g_sai_buffer_db_ptr, g_sai_buffer_db_size, MS_SYNC);
    return SAI_STATUS_SUCCESS;
}

static void sai_buffer_db_data_reset()
{
    if (0 == g_sai_buffer_db_size) {
        g_sai_buffer_db_size = sai_buffer_db_size_get();
    }
    memset(g_sai_buffer_db_ptr->db_base_ptr, 0, g_sai_buffer_db_size);
}

static void sai_buffer_db_pointers_init()
{
    assert(g_sai_db_ptr->ports_number != 0);
    g_sai_buffer_db_ptr->buffer_profiles  = (mlnx_sai_db_buffer_profile_entry_t*)(g_sai_buffer_db_ptr->db_base_ptr);
    g_sai_buffer_db_ptr->port_buffer_data = (uint32_t*)(g_sai_buffer_db_ptr->buffer_profiles +
                                                        (1 +
                                                         (MAX_PORTS *
                                                          mlnx_sai_get_buffer_resource_limits()->max_buffers_per_port)));
    g_sai_buffer_db_ptr->pool_allocation = (bool*)(g_sai_buffer_db_ptr->port_buffer_data +
                                                   BUFFER_DB_PER_PORT_PROFILE_INDEX_ARRAY_SIZE * MAX_PORTS);
}

static sai_status_t sai_buffer_db_unload(boolean_t erase_db)
{
    int          err    = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (erase_db == TRUE) {
        cl_shm_destroy(SAI_BUFFER_PATH);
    }

    if (g_sai_buffer_db_ptr == NULL) {
        return status;
    }

    if (g_sai_buffer_db_ptr->db_base_ptr != NULL) {
        err = munmap(g_sai_buffer_db_ptr->db_base_ptr, g_sai_buffer_db_size);
        if (err == -1) {
            SX_LOG_ERR("Failed to unmap the shared memory of the SAI buffer DB\n");
            status = SAI_STATUS_FAILURE;
        }
    }
    free(g_sai_buffer_db_ptr);
    g_sai_buffer_db_ptr = NULL;
    return status;
}

static uint32_t sai_buffer_db_size_get()
{
    if (0 == g_sai_db_ptr->ports_number) {
        MLNX_SAI_LOG_ERR("g_sai_db_ptr->ports_number NOT CONFIGURED\n");
        return SAI_STATUS_FAILURE;
    }

    return (
        /*
         *  buffer profiles
         */
        sizeof(mlnx_sai_db_buffer_profile_entry_t) *
        (1 + (MAX_PORTS * mlnx_sai_get_buffer_resource_limits()->max_buffers_per_port)) +

        /*
         *  for each port - 3 arrays holding references to buffer profiles, see comments on sai_buffer_db_t.port_buffer_data
         */
        sizeof(uint32_t) * BUFFER_DB_PER_PORT_PROFILE_INDEX_ARRAY_SIZE * MAX_PORTS +
        /*size for pool db flags + 1 bool field for flag specifying whether has user ever called create_pool function.*/
        sizeof(bool) *
        (1 + mlnx_sai_get_buffer_resource_limits()->num_ingress_pools +
         mlnx_sai_get_buffer_resource_limits()->num_egress_pools) +
        sizeof(mlnx_sai_buffer_pool_ids_t)
        );
}

static sai_status_t sai_buffer_db_create()
{
    int         shmid;
    cl_status_t cl_err;

    init_buffer_resource_limits();

    cl_err = cl_shm_create(SAI_BUFFER_PATH, &shmid);
    if (cl_err) {
        if (errno == EEXIST) { /* one retry is allowed */
            MLNX_SAI_LOG_WRN("Shared memory of the SAI buffer already exists, destroying it and re-creating\n");
            cl_shm_destroy(SAI_BUFFER_PATH);
            cl_err = cl_shm_create(SAI_BUFFER_PATH, &shmid);
        }

        if (cl_err) {
            MLNX_SAI_LOG_ERR("Failed to create shared memory for SAI buffer DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }
    }

    g_sai_buffer_db_size = sai_buffer_db_size_get();

    if (ftruncate(shmid, g_sai_buffer_db_size) == -1) {
        MLNX_SAI_LOG_ERR("Failed to set shared memory size for the SAI buffer DB\n");
        cl_shm_destroy(SAI_BUFFER_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_buffer_db_ptr = malloc(sizeof(sai_buffer_db_t));
    if (g_sai_buffer_db_ptr == NULL) {
        MLNX_SAI_LOG_ERR("Failed to allocate SAI buffer DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_buffer_db_ptr->db_base_ptr = mmap(NULL, g_sai_buffer_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_buffer_db_ptr->db_base_ptr == MAP_FAILED) {
        MLNX_SAI_LOG_ERR("Failed to map the shared memory of the SAI buffer DB\n");
        g_sai_buffer_db_ptr->db_base_ptr = NULL;
        cl_shm_destroy(SAI_BUFFER_PATH);
        return SAI_STATUS_NO_MEMORY;
    }
    return SAI_STATUS_SUCCESS;
}

static bool is_prime_number(uint32_t a)
{
    uint32_t max_divisor;
    uint32_t ii;

    max_divisor = (uint32_t)sqrt(a);
    for (ii = 2; ii <= max_divisor; ii++) {
        if (a % ii == 0) {
            return false;
        }
    }

    return true;
}

static uint32_t sai_acl_db_pbs_map_size_get()
{
    uint32_t size;

    size = (uint32_t)(ACL_MAX_PBS_NUMBER * ACL_PBS_MAP_RESERVE_PERCENT);

    if (size % 2 == 0) {
        size++;
    }

    while (!is_prime_number(size)) {
        size += 2;
    }

    return size;
}

static uint32_t sai_acl_db_size_get()
{
    g_sai_acl_db_pbs_map_size = sai_acl_db_pbs_map_size_get();

    return (sizeof(acl_table_db_t) * ACL_TABLE_DB_SIZE +
            sizeof(acl_entry_db_t) * ACL_ENTRY_DB_SIZE +
            sizeof(acl_setting_tbl_t) +
            sizeof(acl_pbs_map_entry_t) * (ACL_PBS_MAP_PREDEF_REG_SIZE + g_sai_acl_db_pbs_map_size) +
            (sizeof(acl_bind_points_db_t) + sizeof(acl_bind_point_t) * ACL_RIF_COUNT) +
            (sizeof(acl_group_db_t) + sizeof(acl_group_member_t) *
             ACL_GROUP_SIZE) * (ACL_GROUP_NUMBER / g_sai_db_ptr->acl_divider) +
            sizeof(acl_vlan_group_t) * ACL_VLAN_GROUP_COUNT) +
           ((sizeof(acl_group_bound_to_t) + (sizeof(acl_bind_point_index_t) * SAI_ACL_MAX_BIND_POINT_BOUND))
            * ACL_GROUP_NUMBER / g_sai_db_ptr->acl_divider) +
           sai_udf_db_size_get();
}

static uint32_t sai_udf_db_size_get()
{
    return MLNX_UDF_DB_UDF_GROUPS_SIZE + MLNX_UDF_DB_UDF_GROUPS_UDFS_SIZE +
           MLNX_UDF_DB_UDFS_SIZE + MLNX_UDF_DB_MATCHES_SIZE;
}

static void sai_acl_db_init()
{
    g_sai_acl_db_ptr->acl_table_db = (acl_table_db_t*)(g_sai_acl_db_ptr->db_base_ptr);

    g_sai_acl_db_ptr->acl_entry_db = (acl_entry_db_t*)((uint8_t*)g_sai_acl_db_ptr->acl_table_db +
                                                       sizeof(acl_table_db_t) * ACL_TABLE_DB_SIZE);

    g_sai_acl_db_ptr->acl_settings_tbl = (acl_setting_tbl_t*)((uint8_t*)g_sai_acl_db_ptr->acl_entry_db +
                                                              sizeof(acl_entry_db_t) * ACL_ENTRY_DB_SIZE);

    g_sai_acl_db_ptr->acl_pbs_map_db = (acl_pbs_map_entry_t*)((uint8_t*)g_sai_acl_db_ptr->acl_settings_tbl +
                                                              sizeof(acl_setting_tbl_t));

    g_sai_acl_db_ptr->acl_bind_points = (acl_bind_points_db_t*)((uint8_t*)g_sai_acl_db_ptr->acl_pbs_map_db +
                                                                (sizeof(acl_pbs_map_entry_t) *
                                                                 (ACL_PBS_MAP_PREDEF_REG_SIZE +
                                                                  g_sai_acl_db_pbs_map_size)));

    g_sai_acl_db_ptr->acl_groups_db = (acl_group_db_t*)((uint8_t*)g_sai_acl_db_ptr->acl_bind_points +
                                                        (sizeof(acl_bind_points_db_t) + sizeof(acl_bind_point_t) *
                                                         ACL_RIF_COUNT));

    g_sai_acl_db_ptr->acl_vlan_groups_db = (acl_vlan_group_t*)((uint8_t*)g_sai_acl_db_ptr->acl_groups_db +
                                                               (sizeof(acl_group_db_t) + sizeof(acl_group_member_t) *
                                                                ACL_GROUP_SIZE) * ACL_GROUP_NUMBER /
                                                               g_sai_db_ptr->acl_divider);

    g_sai_acl_db_ptr->acl_group_bound_to_db = (acl_group_bound_to_t*)((uint8_t*)g_sai_acl_db_ptr->acl_vlan_groups_db +
                                                                      sizeof(acl_vlan_group_t) * ACL_VLAN_GROUP_COUNT);

    sai_udf_db_init();
}

static void sai_udf_db_init()
{
    g_sai_acl_db_ptr->udf_db.groups = (mlnx_udf_group_t*)((uint8_t*)g_sai_acl_db_ptr->acl_group_bound_to_db +
                                                          ((sizeof(acl_group_bound_to_t) +
                                                            (sizeof(acl_bind_point_index_t)
                                                             *
                                                             SAI_ACL_MAX_BIND_POINT_BOUND)) * ACL_GROUP_NUMBER /
                                                           g_sai_db_ptr->acl_divider));

    g_sai_acl_db_ptr->udf_db.groups_udfs =
        (mlnx_udf_list_t*)((uint8_t*)g_sai_acl_db_ptr->udf_db.groups + MLNX_UDF_DB_UDF_GROUPS_SIZE);

    g_sai_acl_db_ptr->udf_db.udfs =
        (mlnx_udf_t*)((uint8_t*)g_sai_acl_db_ptr->udf_db.groups_udfs + MLNX_UDF_DB_UDF_GROUPS_UDFS_SIZE);

    g_sai_acl_db_ptr->udf_db.matches =
        (mlnx_match_t*)((uint8_t*)g_sai_acl_db_ptr->udf_db.udfs + MLNX_UDF_DB_UDFS_SIZE);
}

static sai_status_t sai_acl_db_create()
{
    int         shmid;
    cl_status_t cl_err;

    cl_err = cl_shm_create(SAI_ACL_PATH, &shmid);
    if (cl_err) {
        if (errno == EEXIST) {
            MLNX_SAI_LOG_WRN("Shared memory of the SAI ACL already exists, destroying it and re-creating\n");
            cl_shm_destroy(SAI_ACL_PATH);
            cl_err = cl_shm_create(SAI_ACL_PATH, &shmid);
        }

        if (cl_err) {
            MLNX_SAI_LOG_ERR("Failed to create shared memory for SAI ACL DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }
    }

    g_sai_acl_db_size = sai_acl_db_size_get();

    if (ftruncate(shmid, g_sai_acl_db_size) == -1) {
        MLNX_SAI_LOG_ERR("Failed to set shared memory size for the SAI ACL DB\n");
        cl_shm_destroy(SAI_ACL_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_acl_db_ptr = malloc(sizeof(*g_sai_acl_db_ptr));
    if (g_sai_acl_db_ptr == NULL) {
        MLNX_SAI_LOG_ERR("Failed to allocate SAI ACL DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_acl_db_ptr->db_base_ptr = mmap(NULL, g_sai_acl_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_acl_db_ptr->db_base_ptr == MAP_FAILED) {
        MLNX_SAI_LOG_ERR("Failed to map the shared memory of the SAI ACL DB\n");
        g_sai_acl_db_ptr->db_base_ptr = NULL;
        cl_shm_destroy(SAI_ACL_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    memset(g_sai_acl_db_ptr->db_base_ptr, 0, g_sai_acl_db_size);

    return SAI_STATUS_SUCCESS;
}

static sai_status_t sai_acl_db_unload(boolean_t erase_db)
{
    int          err    = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (erase_db == TRUE) {
        cl_shm_destroy(SAI_ACL_PATH);
    }

    if (g_sai_acl_db_ptr == NULL) {
        return status;
    }

    if (g_sai_acl_db_ptr->db_base_ptr != NULL) {
        err = munmap(g_sai_acl_db_ptr->db_base_ptr, g_sai_acl_db_size);
        if (err == -1) {
            SX_LOG_ERR("Failed to unmap the shared memory of the SAI ACL DB\n");
            status = SAI_STATUS_FAILURE;
        }
    }

    free(g_sai_acl_db_ptr);
    g_sai_acl_db_ptr = NULL;

    return status;
}

static uint32_t sai_tunnel_db_size_get()
{
    return (sizeof(mlnx_tunneltable_t) * MLNX_TUNNELTABLE_SIZE +
            sizeof(mlnx_tunnel_entry_t) * MAX_TUNNEL_DB_SIZE +
            sizeof(mlnx_tunnel_map_t) * MLNX_TUNNEL_MAP_MAX +
            sizeof(mlnx_tunnel_map_entry_t) * MLNX_TUNNEL_MAP_ENTRY_MAX);
}

static void sai_tunnel_db_init()
{
    g_sai_tunnel_db_ptr->tunneltable_db = (mlnx_tunneltable_t*)(g_sai_tunnel_db_ptr->db_base_ptr);

    g_sai_tunnel_db_ptr->tunnel_entry_db = (mlnx_tunnel_entry_t*)((uint8_t*)g_sai_tunnel_db_ptr->tunneltable_db +
                                                                  sizeof(mlnx_tunneltable_t) * MLNX_TUNNELTABLE_SIZE);

    g_sai_tunnel_db_ptr->tunnel_map_db = (mlnx_tunnel_map_t*)((uint8_t*)g_sai_tunnel_db_ptr->tunnel_entry_db +
                                                              sizeof(mlnx_tunnel_entry_t) * MAX_TUNNEL_DB_SIZE);

    g_sai_tunnel_db_ptr->tunnel_map_entry_db =
        (mlnx_tunnel_map_entry_t*)((uint8_t*)g_sai_tunnel_db_ptr->tunnel_map_db +
                                   sizeof(mlnx_tunnel_map_t) *
                                   MLNX_TUNNEL_MAP_MAX);
}

static sai_status_t sai_tunnel_db_create()
{
    int         shmid;
    cl_status_t cl_err;

    cl_err = cl_shm_create(SAI_TUNNEL_PATH, &shmid);
    if (cl_err) {
        if (errno == EEXIST) {
            MLNX_SAI_LOG_WRN("Shared memory of the SAI TUNNEL already exists, destroying it and re-creating\n");
            cl_shm_destroy(SAI_TUNNEL_PATH);
            cl_err = cl_shm_create(SAI_TUNNEL_PATH, &shmid);
        }

        if (cl_err) {
            MLNX_SAI_LOG_ERR("Failed to create shared memory for SAI TUNNEL DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }
    }

    g_sai_tunnel_db_size = sai_tunnel_db_size_get();

    if (ftruncate(shmid, g_sai_tunnel_db_size) == -1) {
        MLNX_SAI_LOG_ERR("Failed to set shared memory size for the SAI TUNNEL DB\n");
        cl_shm_destroy(SAI_TUNNEL_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_tunnel_db_ptr = malloc(sizeof(*g_sai_tunnel_db_ptr));
    if (g_sai_tunnel_db_ptr == NULL) {
        MLNX_SAI_LOG_ERR("Failed to allocate SAI TUNNEL DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_tunnel_db_ptr->db_base_ptr = mmap(NULL, g_sai_tunnel_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_tunnel_db_ptr->db_base_ptr == MAP_FAILED) {
        MLNX_SAI_LOG_ERR("Failed to map the shared memory of the SAI TUNNEL DB\n");
        g_sai_tunnel_db_ptr->db_base_ptr = NULL;
        cl_shm_destroy(SAI_TUNNEL_PATH);
        return SAI_STATUS_NO_MEMORY;
    }

    memset(g_sai_tunnel_db_ptr->db_base_ptr, 0, g_sai_tunnel_db_size);

    return SAI_STATUS_SUCCESS;
}

static sai_status_t sai_tunnel_db_unload(boolean_t erase_db)
{
    int          err    = 0;
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (erase_db == TRUE) {
        cl_shm_destroy(SAI_TUNNEL_PATH);
    }

    if (g_sai_tunnel_db_ptr == NULL) {
        return status;
    }

    if (g_sai_tunnel_db_ptr->db_base_ptr != NULL) {
        err = munmap(g_sai_tunnel_db_ptr->db_base_ptr, g_sai_tunnel_db_size);
        if (err == -1) {
            SX_LOG_ERR("Failed to unmap the shared memory of the SAI TUNNEL DB\n");
            status = SAI_STATUS_FAILURE;
        }
    }

    free(g_sai_tunnel_db_ptr);
    g_sai_tunnel_db_ptr = NULL;

    return status;
}

static sai_status_t sai_tunnel_db_switch_connect_init(int shmid)
{
    g_sai_tunnel_db_size = sai_tunnel_db_size_get();
    g_sai_tunnel_db_ptr  = malloc(sizeof(*g_sai_tunnel_db_ptr));
    if (g_sai_tunnel_db_ptr == NULL) {
        SX_LOG_ERR("Failed to allocate SAI Tunnel DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_tunnel_db_ptr->db_base_ptr = mmap(NULL, g_sai_tunnel_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_tunnel_db_ptr->db_base_ptr == MAP_FAILED) {
        SX_LOG_ERR("Failed to map the shared memory of the SAI Tunnel DB\n");
        g_sai_tunnel_db_ptr->db_base_ptr = NULL;
        return SAI_STATUS_NO_MEMORY;
    }

    sai_tunnel_db_init();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t sai_acl_db_switch_connect_init(int shmid)
{
    g_sai_acl_db_size = sai_acl_db_size_get();
    g_sai_acl_db_ptr  = malloc(sizeof(*g_sai_acl_db_ptr));
    if (g_sai_acl_db_ptr == NULL) {
        SX_LOG_ERR("Failed to allocate SAI ACL DB structure\n");
        return SAI_STATUS_NO_MEMORY;
    }

    g_sai_acl_db_ptr->db_base_ptr = mmap(NULL, g_sai_acl_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
    if (g_sai_acl_db_ptr->db_base_ptr == MAP_FAILED) {
        SX_LOG_ERR("Failed to map the shared memory of the SAI ACL DB\n");
        g_sai_acl_db_ptr->db_base_ptr = NULL;
        return SAI_STATUS_NO_MEMORY;
    }

    sai_acl_db_init();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_cb_table_init(void)
{
    sai_status_t status;

    status = mlnx_port_cb_table_init();
    if (SAI_ERR(status)) {
        return status;
    }

    status = mlnx_acl_cb_table_init();
    if (SAI_ERR(status)) {
        return status;
    }

    return status;
}

static sai_status_t mlnx_sai_rm_initialize(const char *config_file)
{
    sai_status_t     status;
    sxd_chip_types_t chip_type;

    status = get_chip_type(&chip_type);
    if (SX_ERR(status)) {
        SX_LOG_ERR("get_chip_type failed\n");
        return SAI_STATUS_FAILURE;
    }

    MLNX_SAI_LOG_DBG("Chip type - %s\n", SX_CHIP_TYPE_STR(chip_type));

    status = rm_chip_limits_get(chip_type, &g_resource_limits);
    if (SX_ERR(status)) {
        MLNX_SAI_LOG_ERR("Failed to get chip resources - %s.\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    status = mlnx_sai_db_initialize(config_file, chip_type);
    if (SAI_ERR(status)) {
        return status;
    }

    status = mlnx_cb_table_init();
    if (SAI_ERR(status)) {
        return status;
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_kvd_table_size_update(sx_api_profile_t *ku_profile)
{
    const char *fdb_table_size, *route_table_size, *ipv4_route_table_size, *ipv6_route_table_size;
    const char *neighbor_table_size, *ipv4_neigh_table_size, *ipv6_neigh_table_size;
    uint32_t    kvd_table_size, hash_single_size, hash_double_size;
    uint32_t    fdb_num        = 0;
    uint32_t    routes_num     = 0, ipv4_routes_num = 0, ipv6_routes_num = 0;
    uint32_t    neighbors_num  = 0, ipv4_neighbors_num = 0, ipv6_neighbors_num = 0;
    bool        update_profile = false;

    kvd_table_size = ku_profile->kvd_hash_single_size + ku_profile->kvd_hash_double_size +
                     ku_profile->kvd_linear_size;
    hash_single_size = ku_profile->kvd_hash_single_size;
    hash_double_size = ku_profile->kvd_hash_double_size;

    MLNX_SAI_LOG_NTC("Original hash_single size %u hash_double size %u \n", hash_single_size, hash_double_size);

    g_fdb_table_size           = hash_single_size;
    g_route_table_size         = hash_single_size;
    g_neighbor_table_size      = hash_single_size;
    g_ipv4_route_table_size    = 0;
    g_ipv6_route_table_size    = 0;
    g_ipv4_neighbor_table_size = 0;
    g_ipv6_neighbor_table_size = 0;
    fdb_table_size             = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_FDB_TABLE_SIZE);
    route_table_size           = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_L3_ROUTE_TABLE_SIZE);
    neighbor_table_size        = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_L3_NEIGHBOR_TABLE_SIZE);
    ipv4_route_table_size      = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_IPV4_ROUTE_TABLE_SIZE);
    ipv6_route_table_size      = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_IPV6_ROUTE_TABLE_SIZE);
    ipv4_neigh_table_size      = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_IPV4_NEIGHBOR_TABLE_SIZE);
    ipv6_neigh_table_size      = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_IPV6_NEIGHBOR_TABLE_SIZE);

    if (NULL != fdb_table_size) {
        fdb_num = (uint32_t)atoi(fdb_table_size);
        MLNX_SAI_LOG_NTC("Setting initial fdb table size %u\n", fdb_num);
        /* 0 is full kvd */
        if (fdb_num) {
            g_fdb_table_size = fdb_num;
        }
    }

    if (NULL != route_table_size) {
        routes_num = (uint32_t)atoi(route_table_size);
        MLNX_SAI_LOG_NTC("Setting initial route table size %u\n", routes_num);
        /* 0 is full kvd */
        if (routes_num) {
            g_route_table_size      = routes_num;
            g_ipv4_route_table_size = routes_num;
            g_ipv6_route_table_size = routes_num;
        }
    } else {
        if (NULL != ipv4_route_table_size) {
            ipv4_routes_num = (uint32_t)atoi(ipv4_route_table_size);
            MLNX_SAI_LOG_NTC("Setting initial IPv4 route table size %u\n", ipv4_routes_num);
            if (ipv4_routes_num) {
                g_ipv4_route_table_size = ipv4_routes_num;
            }
        }
        if (NULL != ipv6_route_table_size) {
            ipv6_routes_num = (uint32_t)atoi(ipv6_route_table_size);
            MLNX_SAI_LOG_NTC("Setting initial IPv6 route table size %u\n", ipv6_routes_num);
            if (ipv6_routes_num) {
                g_ipv6_route_table_size = ipv6_routes_num;
            }
        }
        if (ipv4_route_table_size && ipv6_route_table_size) {
            g_route_table_size = g_ipv4_route_table_size + g_ipv6_route_table_size;
        }
    }

    if (NULL != neighbor_table_size) {
        neighbors_num = (uint32_t)atoi(neighbor_table_size);
        MLNX_SAI_LOG_NTC("Setting initial neighbor table size %u\n", neighbors_num);
        if (neighbors_num) {
            g_neighbor_table_size      = neighbors_num;
            g_ipv4_neighbor_table_size = neighbors_num;
            g_ipv6_neighbor_table_size = neighbors_num;
        }
    } else {
        if (NULL != ipv4_neigh_table_size) {
            ipv4_neighbors_num = (uint32_t)atoi(ipv4_neigh_table_size);
            MLNX_SAI_LOG_NTC("Setting initial IPv4 neighbor table size %u\n", ipv4_neighbors_num);
            if (ipv4_neighbors_num) {
                g_ipv4_neighbor_table_size = ipv4_neighbors_num;
            }
        }
        if (NULL != ipv6_neigh_table_size) {
            ipv6_neighbors_num = (uint32_t)atoi(ipv6_neigh_table_size);
            MLNX_SAI_LOG_NTC("Setting initial IPv6 neighbor table size %u\n", ipv6_neighbors_num);
            if (ipv6_neighbors_num) {
                g_ipv6_neighbor_table_size = ipv6_neighbors_num;
            }
        }
        if (ipv4_neigh_table_size && ipv6_neigh_table_size) {
            g_neighbor_table_size = g_ipv4_neighbor_table_size + g_ipv6_neighbor_table_size;
        }
    }

    /* Will update the profile only when the size of fdb/route/neighbor are all set */
    if (fdb_num && routes_num && neighbors_num) {
        hash_single_size = fdb_num + routes_num + neighbors_num;
        hash_double_size = (routes_num + neighbors_num) * 2;
        update_profile   = true;
    } else {
        if (fdb_num && ipv4_routes_num && ipv4_neighbors_num) {
            hash_single_size = fdb_num + ipv4_routes_num + ipv4_neighbors_num;
            update_profile   = true;
        }
        if (ipv6_routes_num && ipv6_neighbors_num) {
            hash_double_size = (ipv6_routes_num + ipv6_neighbors_num) * 2;
            update_profile   = true;
        }
    }

    if (update_profile) {
        /*
         * When in ISSU mode, SDK initializes kvd with cutting kvd_hash_single_size and kvd_hash_single_size by half.
         * We multiply it by 2 to initialize with the exact values user specified.
         */
        if (g_sai_db_ptr->issu_enabled) {
            hash_single_size *= 2;
            hash_double_size *= 2;

            SX_LOG_NTC("Doubling the hash_single and hash_double sizes (ISSU is enabled): "
                       "hash_single_size %u -> %u, hash_double_size %u -> %u\n",
                       hash_single_size / 2, hash_single_size, hash_double_size / 2, hash_double_size);
        }

        if (hash_single_size + hash_double_size <= kvd_table_size) {
            ku_profile->kvd_hash_single_size = hash_single_size;
            ku_profile->kvd_hash_double_size = hash_double_size;
            ku_profile->kvd_linear_size      = kvd_table_size - hash_single_size - hash_double_size;
            MLNX_SAI_LOG_NTC("New hash_single size %u hash_double size %u linear size %u\n",
                             ku_profile->kvd_hash_single_size, ku_profile->kvd_hash_double_size,
                             ku_profile->kvd_linear_size);
        } else {
            MLNX_SAI_LOG_NTC(
                "Hash table size is more than kvd size: hash_single size %u, hash_double size %u,"
                "kvd size %u. Don't update the profile\n",
                hash_single_size, hash_double_size, kvd_table_size);
        }
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_initialize_switch(sai_object_id_t switch_id, bool *transaction_mode_enable)
{
    int                         system_err;
    const char                 *config_file;
    const char                 *boot_type_char;
    mlnx_sai_boot_type_t        boot_type = 0;
    sx_router_resources_param_t resources_param;
    sx_router_general_param_t   general_param;
    sx_status_t                 sdk_status;
    sai_status_t                sai_status;
    sx_api_profile_t           *ku_profile;
    sx_tunnel_attribute_t       sx_tunnel_attribute;
    const bool                  warm_recover = false;
    bool                        issu_enabled = false;
    cl_status_t                 cl_err;
    sx_router_attributes_t      router_attr;
    sx_router_id_t              vrid;
    sx_span_init_params_t       span_init_params;
    sx_flex_parser_param_t      flex_parser_param;

    memset(&span_init_params, 0, sizeof(sx_span_init_params_t));
    memset(&flex_parser_param, 0, sizeof(sx_flex_parser_param_t));

    assert(sizeof(sx_tunnel_attribute.attributes.ipinip_p2p) ==
           sizeof(sx_tunnel_attribute.attributes.ipinip_p2p_gre));

    if (NULL == transaction_mode_enable) {
        MLNX_SAI_LOG_ERR("transaction mode enable is null\n");
        return SAI_STATUS_FAILURE;
    }

    config_file = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_INIT_CONFIG_FILE);
    if (NULL == config_file) {
        MLNX_SAI_LOG_ERR("NULL config file for profile %u\n", g_profile_id);
        return SAI_STATUS_INVALID_PARAMETER;
    }

    system_err = system("pidof sx_sdk");
    if (0 == system_err) {
        MLNX_SAI_LOG_ERR("SDK already running. Please terminate it before running SAI init.\n");
        return SAI_STATUS_FAILURE;
    }

    boot_type_char = g_mlnx_services.profile_get_value(g_profile_id, SAI_KEY_BOOT_TYPE);
    if (NULL != boot_type_char) {
        boot_type = (uint8_t)atoi(boot_type_char);
    } else {
        boot_type = 0;
    }

    switch (boot_type) {
    case BOOT_TYPE_REGULAR:
#if (!defined ACS_OS) || (defined ACS_OS_NO_DOCKERS)
        system_err = system("/etc/init.d/sxdkernel start");
        if (0 != system_err) {
            MLNX_SAI_LOG_ERR("Failed running sxdkernel start.\n");
            return SAI_STATUS_FAILURE;
        }
#endif
        break;

    case BOOT_TYPE_WARM:
    case BOOT_TYPE_FAST:
#if (!defined ACS_OS) || (defined ACS_OS_NO_DOCKERS)
        system_err = system("env FAST_BOOT=1 /etc/init.d/sxdkernel start");
        if (0 != system_err) {
            MLNX_SAI_LOG_ERR("Failed running sxdkernel start.\n");
            return SAI_STATUS_FAILURE;
        }
#endif
        break;

    /* default */
    default:
        MLNX_SAI_LOG_ERR("Boot type %d not recognized, must be 0 (cold) or 1 (warm) or 2 (fast)\n", boot_type);
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_status = mlnx_sai_rm_initialize(config_file);
    if (SAI_ERR(sai_status)) {
        return sai_status;
    }

    sai_status = mlnx_sdk_start(boot_type);
    if (SAI_ERR(sai_status)) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_resource_mng_stage(warm_recover, boot_type))) {
        return sai_status;
    }

    if ((BOOT_TYPE_FAST == boot_type) && (!(*transaction_mode_enable))) {
        MLNX_SAI_LOG_ERR("Transaction mode should be enabled, enabling now\n");
        *transaction_mode_enable = true;
    }

    sai_db_write_lock();
    g_sai_db_ptr->boot_type = boot_type;
    issu_enabled            = g_sai_db_ptr->issu_enabled;
    sai_db_unlock();

    ku_profile = mlnx_sai_get_ku_profile();
    if (!ku_profile) {
        return SAI_STATUS_FAILURE;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_kvd_table_size_update(ku_profile))) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_chassis_mng_stage(boot_type,
                                                                   *transaction_mode_enable,
                                                                   ku_profile))) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_dvs_mng_stage(boot_type, switch_id))) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = switch_open_traps())) {
        return sai_status;
    }

    cl_err = cl_thread_init(&event_thread, event_thread_func, (const void*const)switch_id, NULL);
    if (cl_err) {
        SX_LOG_ERR("Failed to create event thread\n");
        return SAI_STATUS_FAILURE;
    }

    /* init router model */
    memset(&resources_param, 0, sizeof(resources_param));
    memset(&general_param, 0, sizeof(general_param));

    if (issu_enabled) {
        resources_param.max_virtual_routers_num = g_resource_limits.router_vrid_max / 2;
    } else {
        resources_param.max_virtual_routers_num = g_resource_limits.router_vrid_max;
    }
    if (issu_enabled) {
        resources_param.max_router_interfaces = g_resource_limits.router_rifs_max / 2;
    } else {
        resources_param.max_router_interfaces = g_resource_limits.router_rifs_max;
    }

    resources_param.min_ipv4_uc_route_entries = g_ipv4_route_table_size;
    resources_param.min_ipv6_uc_route_entries = g_ipv6_route_table_size;
    resources_param.max_ipv4_uc_route_entries = g_ipv4_route_table_size;
    resources_param.max_ipv6_uc_route_entries = g_ipv6_route_table_size;

    resources_param.min_ipv4_neighbor_entries = g_ipv4_neighbor_table_size;
    resources_param.min_ipv6_neighbor_entries = g_ipv6_neighbor_table_size;
    resources_param.max_ipv4_neighbor_entries = g_ipv4_neighbor_table_size;
    resources_param.max_ipv6_neighbor_entries = g_ipv6_neighbor_table_size;

    resources_param.min_ipv4_mc_route_entries = 0;
    resources_param.min_ipv6_mc_route_entries = 0;
    resources_param.max_ipv4_mc_route_entries = 0;
    resources_param.max_ipv6_mc_route_entries = 0;

#ifdef ACS_OS
    resources_param.max_ipinip_ipv6_loopback_rifs = 12;
#else
    /* sdk limit in sdk_router_be_validate_params */
    resources_param.max_ipinip_ipv6_loopback_rifs = resources_param.max_virtual_routers_num / 2;
#endif

    general_param.ipv4_enable    = 1;
    general_param.ipv6_enable    = 1;
    general_param.ipv4_mc_enable = 0;
    general_param.ipv6_mc_enable = 0;
    general_param.rpf_enable     = 0;

    memset(&router_attr, 0, sizeof(router_attr));

    router_attr.ipv4_enable            = 1;
    router_attr.ipv6_enable            = 1;
    router_attr.ipv4_mc_enable         = 0;
    router_attr.ipv6_mc_enable         = 0;
    router_attr.uc_default_rule_action = SX_ROUTER_ACTION_DROP;
    router_attr.mc_default_rule_action = SX_ROUTER_ACTION_DROP;

    if (SX_STATUS_SUCCESS != (sdk_status = sx_api_router_init_set(gh_sdk, &general_param, &resources_param))) {
        SX_LOG_ERR("Router init failed - %s.\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    if (SX_STATUS_SUCCESS != (sdk_status = sx_api_router_set(gh_sdk, SX_ACCESS_CMD_ADD, &router_attr, &vrid))) {
        SX_LOG_ERR("Failed to add default router - %s.\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    /* Update route/neighbor table size to default value if no setting in profile.
     * SDK counts v4 and v6 on same HW table KVD HASH, so init for available resource
     * checking to same value. */
    if (g_ipv4_route_table_size == 0) {
        g_ipv4_route_table_size = ku_profile->kvd_hash_single_size;
    }
    if (g_ipv6_route_table_size == 0) {
        g_ipv6_route_table_size = ku_profile->kvd_hash_single_size;
    }
    if (g_ipv4_neighbor_table_size == 0) {
        g_ipv4_neighbor_table_size = ku_profile->kvd_hash_single_size;
    }
    if (g_ipv6_neighbor_table_size == 0) {
        g_ipv6_neighbor_table_size = ku_profile->kvd_hash_single_size;
    }

    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);
    sai_status = mlnx_create_object(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, vrid, NULL, &g_sai_db_ptr->default_vrid);
    msync(g_sai_db_ptr, sizeof(*g_sai_db_ptr), MS_SYNC);
    cl_plock_release(&g_sai_db_ptr->p_lock);
    if (SAI_STATUS_SUCCESS != sai_status) {
        return sai_status;
    }

    /* Set default aging time - 0 (disabled) */
    if (SX_STATUS_SUCCESS !=
        (sdk_status = sx_api_fdb_age_time_set(gh_sdk, DEFAULT_ETH_SWID, SX_FDB_AGE_TIME_MAX))) {
        SX_LOG_ERR("Failed to set fdb age time - %s.\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_init_buffer_pool_ids())) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_hash_initialize())) {
        return sai_status;
    }

    if (SAI_STATUS_SUCCESS != (sai_status = mlnx_acl_init())) {
        SX_LOG_ERR("Failed to init acl DB\n");
        return sai_status;
    }

    span_init_params.version = SX_SPAN_MIRROR_HEADER_VERSION_1;

    if (SX_STATUS_SUCCESS !=
        (sdk_status = sx_api_span_init_set(gh_sdk, &span_init_params))) {
        SX_LOG_ERR("Failed to init SPAN: %s\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    sai_status = mlnx_bridge_init();
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Failed initialize default bridge\n");
        return sai_status;
    }

    sai_status = mlnx_default_vlan_flood_ctrl_init();
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Failed to init flood control configuration for default VLAN\n");
        return sai_status;
    }

    if (SX_STATUS_SUCCESS !=
        (sdk_status = sx_api_flex_parser_init_set(gh_sdk, &flex_parser_param))) {
        SX_LOG_ERR("Failed to init flex parser: %s\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    if (SX_STATUS_SUCCESS !=
        (sdk_status = sx_api_flex_parser_log_verbosity_level_set(gh_sdk,
                                                                 SX_LOG_VERBOSITY_BOTH,
                                                                 LOG_VAR_NAME(__MODULE__),
                                                                 LOG_VAR_NAME(__MODULE__)))) {
        SX_LOG_ERR("Set flex log verbosity failed - %s.\n", SX_STATUS_MSG(sdk_status));
        return sdk_to_sai(sdk_status);
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_connect_switch(sai_object_id_t switch_id)
{
    int              err, shmid;
    sxd_chip_types_t chip_type;
    sx_status_t      status;
    sxd_status_t     sxd_status;

    /* Open an handle if not done already on init for init agent */
    if (0 == gh_sdk) {
        if (SX_STATUS_SUCCESS != (status = sx_api_open(sai_log_cb, &gh_sdk))) {
            MLNX_SAI_LOG_ERR("Can't open connection to SDK - %s.\n", SX_STATUS_MSG(status));
            return sdk_to_sai(status);
        }

        if (SX_STATUS_SUCCESS != (status = sx_api_system_log_verbosity_level_set(gh_sdk,
                                                                                 SX_LOG_VERBOSITY_TARGET_API,
                                                                                 LOG_VAR_NAME(__MODULE__),
                                                                                 LOG_VAR_NAME(__MODULE__)))) {
            SX_LOG_ERR("Set system log verbosity failed - %s.\n", SX_STATUS_MSG(status));
            return sdk_to_sai(status);
        }

        status = get_chip_type(&chip_type);
        if (SX_ERR(status)) {
            SX_LOG_ERR("get_chip_type failed\n");
            return SAI_STATUS_FAILURE;
        }

        status = rm_chip_limits_get(chip_type, &g_resource_limits);
        if (SX_ERR(status)) {
            SX_LOG_ERR("Failed to get chip resources - %s.\n", SX_STATUS_MSG(status));
            return sdk_to_sai(status);
        }

        g_mlnx_shm_rm_size = (uint32_t)mlnx_sai_rm_db_size_get();

        err = cl_shm_open(SAI_PATH, &shmid);
        if (err) {
            SX_LOG_ERR("Failed to open shared memory of SAI DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }

        g_sai_db_ptr = mmap(NULL,
                            sizeof(*g_sai_db_ptr) + g_mlnx_shm_rm_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED,
                            shmid,
                            0);
        if (g_sai_db_ptr == MAP_FAILED) {
            SX_LOG_ERR("Failed to map the shared memory of the SAI DB\n");
            g_sai_db_ptr = NULL;
            return SAI_STATUS_NO_MEMORY;
        }

        err = cl_shm_open(SAI_QOS_PATH, &shmid);
        if (err) {
            SX_LOG_ERR("Failed to open shared memory of SAI QOS DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }

        g_sai_qos_db_size = sai_qos_db_size_get();
        g_sai_qos_db_ptr  = malloc(sizeof(*g_sai_qos_db_ptr));
        if (g_sai_qos_db_ptr == NULL) {
            SX_LOG_ERR("Failed to allocate SAI QoS DB structure\n");
            return SAI_STATUS_NO_MEMORY;
        }

        g_sai_qos_db_ptr->db_base_ptr = mmap(NULL, g_sai_qos_db_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmid, 0);
        if (g_sai_qos_db_ptr->db_base_ptr == MAP_FAILED) {
            SX_LOG_ERR("Failed to map the shared memory of the SAI QOS DB\n");
            g_sai_qos_db_ptr->db_base_ptr = NULL;
            return SAI_STATUS_NO_MEMORY;
        }

        sai_qos_db_init();

        err = cl_shm_open(SAI_BUFFER_PATH, &shmid);
        if (err) {
            SX_LOG_ERR("Failed to open shared memory of SAI Buffers DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }

        status = sai_buffer_db_switch_connect_init(shmid);
        if (SAI_STATUS_SUCCESS != status) {
            SX_LOG_ERR("Failed to map SAI buffer db on switch connect\n");
            return status;
        }

        err = cl_shm_open(SAI_ACL_PATH, &shmid);
        if (err) {
            SX_LOG_ERR("Failed to open shared memory of SAI ACL DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }

        status = sai_acl_db_switch_connect_init(shmid);
        if (SAI_STATUS_SUCCESS != status) {
            SX_LOG_ERR("Failed to map SAI ACL db on switch connect\n");
            return status;
        }

        err = cl_shm_open(SAI_TUNNEL_PATH, &shmid);
        if (err) {
            SX_LOG_ERR("Failed to open shared memory of SAI Tunnel DB %s\n", strerror(errno));
            return SAI_STATUS_NO_MEMORY;
        }

        status = sai_tunnel_db_switch_connect_init(shmid);
        if (SAI_STATUS_SUCCESS != status) {
            SX_LOG_ERR("Failed to map SAI Tunnel db on switch connect\n");
            return status;
        }

        status = mlnx_cb_table_init();
        if (SAI_ERR(status)) {
            return status;
        }

        sxd_status = sxd_access_reg_init(0, sai_log_cb, LOG_VAR_NAME(__MODULE__));
        if (SXD_CHECK_FAIL(sxd_status)) {
            SX_LOG_ERR("Failed to init access reg - %s.\n", SXD_STATUS_MSG(sxd_status));
            return SAI_STATUS_FAILURE;
        }
    }

    SX_LOG_NTC("Connect switch\n");

    return SAI_STATUS_SUCCESS;
}

/**
 * @brief Create switch
 *
 *   SDK initialization/connect to SDK. After the call the capability attributes should be
 *   ready for retrieval via sai_get_switch_attribute(). Same Switch Object id should be
 *   given for create/connect for each NPU.
 *
 * @param[out] switch_id The Switch Object ID
 * @param[in] attr_count number of attributes
 * @param[in] attr_list Array of attributes
 *
 * @return #SAI_STATUS_SUCCESS on success Failure status code on error
 */
static sai_status_t mlnx_create_switch(_Out_ sai_object_id_t     * switch_id,
                                       _In_ uint32_t               attr_count,
                                       _In_ const sai_attribute_t *attr_list)
{
    char                         list_str[MAX_LIST_VALUE_STR_LEN];
    const sai_attribute_value_t *attr_val       = NULL;
    mlnx_object_id_t             mlnx_switch_id = {0};
    sai_status_t                 sai_status;
    uint32_t                     attr_idx;
    bool                         transaction_mode_enable = false, crc_check_enable = true, crc_recalc_enable = true;

    if (NULL == switch_id) {
        MLNX_SAI_LOG_ERR("NULL switch_id id param\n");
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_status = check_attribs_metadata(attr_count, attr_list, SAI_OBJECT_TYPE_SWITCH, switch_vendor_attribs,
                                        SAI_COMMON_API_CREATE);
    if (SAI_ERR(sai_status)) {
        MLNX_SAI_LOG_ERR("Failed attribs check\n");
        return sai_status;
    }

    sai_attr_list_to_str(attr_count, attr_list, SAI_OBJECT_TYPE_SWITCH, MAX_LIST_VALUE_STR_LEN, list_str);
    MLNX_SAI_LOG_NTC("Create switch, %s\n", list_str);

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_INIT_SWITCH, &attr_val, &attr_idx);
    assert(!SAI_ERR(sai_status));
    mlnx_switch_id.id.is_created = attr_val->booldata;

    sai_status = mlnx_object_id_to_sai(SAI_OBJECT_TYPE_SWITCH, &mlnx_switch_id, switch_id);
    if (SAI_ERR(sai_status)) {
        return sai_status;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_SWITCH_PROFILE_ID, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_profile_id = attr_val->u32;
    }

    sai_status = find_attrib_in_list(attr_count,
                                     attr_list,
                                     SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY,
                                     &attr_val,
                                     &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_bfd_session_state_change = (sai_bfd_session_state_change_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count,
                                     attr_list,
                                     SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY,
                                     &attr_val,
                                     &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_switch_state_change = (sai_switch_state_change_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count,
                                     attr_list,
                                     SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY,
                                     &attr_val,
                                     &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_switch_shutdown_request =
            (sai_switch_shutdown_request_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_fdb_event = (sai_fdb_event_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count,
                                     attr_list,
                                     SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY,
                                     &attr_val,
                                     &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_port_state_change = (sai_port_state_change_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_notification_callbacks.on_packet_event = (sai_packet_event_notification_fn)attr_val->ptr;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_FAST_API_ENABLE, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status) && attr_val->booldata) {
        transaction_mode_enable = true;
    } else {
        transaction_mode_enable = false;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_CRC_CHECK_ENABLE, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        crc_check_enable = attr_val->booldata;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_CRC_RECALCULATION_ENABLE,
                                     &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        crc_recalc_enable = attr_val->booldata;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL,
                                     &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        g_uninit_data_plane_on_removal = attr_val->booldata;
    }
    if (mlnx_switch_id.id.is_created) {
        sai_status = mlnx_initialize_switch(*switch_id, &transaction_mode_enable);
    } else {
        sai_status = mlnx_connect_switch(*switch_id);
    }

    if (SAI_ERR(sai_status)) {
        return sai_status;
    }

    /*
     * Temprorary.
     * Inits the sai_attr_metadata_t structures for ACL UDF attributes
     */
    mlnx_udf_acl_attrs_metadata_init();

    sai_db_write_lock();

    g_sai_db_ptr->transaction_mode_enable = transaction_mode_enable;
    g_sai_db_ptr->crc_check_enable        = crc_check_enable;
    g_sai_db_ptr->crc_recalc_enable       = crc_recalc_enable;

    sai_status = mlnx_switch_crc_params_apply(true);
    if (SAI_ERR(sai_status)) {
        return sai_status;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_VXLAN_DEFAULT_PORT,
                                     &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        sai_status = mlnx_switch_vxlan_udp_port_set(attr_val->u16);
        if (SAI_ERR(sai_status)) {
            MLNX_SAI_LOG_ERR("Error setting vxlan udp port value to %d\n", attr_val->u16);
            return sai_status;
        }
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_RESTART_WARM, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        if (attr_val->booldata) {
            sai_status = mlnx_switch_restart_warm();
            if (SAI_STATUS_SUCCESS != sai_status) {
                MLNX_SAI_LOG_ERR("Error restarting warm\n");
                sai_db_unlock();
                return sai_status;
            }
        }
        g_sai_db_ptr->restart_warm = attr_val->booldata;
    }

    sai_status = find_attrib_in_list(attr_count, attr_list, SAI_SWITCH_ATTR_WARM_RECOVER, &attr_val, &attr_idx);
    if (!SAI_ERR(sai_status)) {
        if (attr_val->booldata) {
            sai_status = mlnx_switch_warm_recover(*switch_id);
            if (SAI_STATUS_SUCCESS != sai_status) {
                MLNX_SAI_LOG_ERR("Error warm recovering\n");
                sai_db_unlock();
                return sai_status;
            }
        }
        g_sai_db_ptr->warm_recover = attr_val->booldata;
    }

    sai_db_unlock();

    return mlnx_object_id_to_sai(SAI_OBJECT_TYPE_SWITCH, &mlnx_switch_id, switch_id);
}

static sai_status_t switch_open_traps(void)
{
    uint32_t                   ii;
    sx_trap_group_attributes_t trap_group_attributes;
    sai_status_t               status;
    sx_host_ifc_register_key_t reg;

    memset(&trap_group_attributes, 0, sizeof(trap_group_attributes));
    memset(&reg, 0, sizeof(reg));
    trap_group_attributes.truncate_mode = SX_TRUNCATE_MODE_DISABLE;
    trap_group_attributes.truncate_size = 0;
    trap_group_attributes.prio          = DEFAULT_TRAP_GROUP_PRIO;
    reg.key_type                        = SX_HOST_IFC_REGISTER_KEY_TYPE_GLOBAL;

    if (SAI_STATUS_SUCCESS != (status = sx_api_host_ifc_trap_group_ext_set(gh_sdk,
                                                                           SX_ACCESS_CMD_SET,
                                                                           DEFAULT_ETH_SWID,
                                                                           DEFAULT_TRAP_GROUP_ID,
                                                                           &trap_group_attributes))) {
        SX_LOG_ERR("Failed to sx_api_host_ifc_trap_group_ext_set %s\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    cl_plock_excl_acquire(&g_sai_db_ptr->p_lock);

    g_sai_db_ptr->trap_group_valid[DEFAULT_TRAP_GROUP_ID] = true;

    if (SAI_STATUS_SUCCESS !=
        (status = mlnx_create_object(SAI_OBJECT_TYPE_HOSTIF_TRAP_GROUP, DEFAULT_TRAP_GROUP_ID, NULL,
                                     &g_sai_db_ptr->default_trap_group))) {
        goto out;
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_host_ifc_open(gh_sdk, &g_sai_db_ptr->callback_channel.channel.fd))) {
        SX_LOG_ERR("host ifc open callback fd failed - %s.\n", SX_STATUS_MSG(status));
        status = sdk_to_sai(status);
        goto out;
    }
    g_sai_db_ptr->callback_channel.type = SX_USER_CHANNEL_TYPE_FD;

    for (ii = 0; END_TRAP_INFO_ID != mlnx_traps_info[ii].trap_id; ii++) {
        g_sai_db_ptr->traps_db[ii].action     = mlnx_traps_info[ii].action;
        g_sai_db_ptr->traps_db[ii].trap_group = g_sai_db_ptr->default_trap_group;

        if (0 == mlnx_traps_info[ii].sdk_traps_num) {
            continue;
        }

        if (mlnx_chip_is_spc2or3()) {
            if ((mlnx_traps_info[ii].trap_id == SAI_HOSTIF_TRAP_TYPE_PTP) ||
                (mlnx_traps_info[ii].trap_id == SAI_HOSTIF_TRAP_TYPE_PTP_TX_EVENT)) {
                continue;
            }
        }

        if (SAI_STATUS_SUCCESS != (status = mlnx_trap_set(ii, mlnx_traps_info[ii].action,
                                                          g_sai_db_ptr->default_trap_group))) {
            goto out;
        }

        if (SAI_STATUS_SUCCESS != (status = mlnx_register_trap(SX_ACCESS_CMD_REGISTER, ii,
                                                               SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_CB,
                                                               g_sai_db_ptr->callback_channel.channel.fd, &reg))) {
            goto out;
        }
    }

#ifdef ACS_OS
    /* Set action NOP for SIP=DIP router ingress discard to allow such traffic */
    {
        sx_status_t             sx_status;
        sx_host_ifc_trap_key_t  trap_key;
        sx_host_ifc_trap_attr_t trap_attr;

        memset(&trap_key, 0, sizeof(trap_key));
        memset(&trap_attr, 0, sizeof(trap_attr));

        trap_key.type                           = HOST_IFC_TRAP_KEY_TRAP_ID_E;
        trap_key.trap_key_attr.trap_id          = SX_TRAP_ID_DISCARD_ING_ROUTER_SIP_DIP;
        trap_attr.attr.trap_id_attr.trap_group  = DEFAULT_TRAP_GROUP_ID;
        trap_attr.attr.trap_id_attr.trap_action = SX_TRAP_ACTION_IGNORE;

        sx_status = sx_api_host_ifc_trap_id_ext_set(gh_sdk, SX_ACCESS_CMD_SET, &trap_key, &trap_attr);
        if (SX_ERR(sx_status)) {
            SX_LOG_ERR("Failed to set SIP_DIP trap action to IGNORE %s\n", SX_STATUS_MSG(sx_status));
            status = sdk_to_sai(sx_status);
            goto out;
        }
    }
#endif

out:
    msync(g_sai_db_ptr, sizeof(*g_sai_db_ptr), MS_SYNC);
    cl_plock_release(&g_sai_db_ptr->p_lock);
    return status;
}

static sai_status_t switch_close_traps(void)
{
    uint32_t ii;

    for (ii = 0; END_TRAP_INFO_ID != mlnx_traps_info[ii].trap_id; ii++) {
        if (0 == mlnx_traps_info[ii].sdk_traps_num) {
            continue;
        }

        /* mlnx_register_trap(SX_ACCESS_CMD_DEREGISTER, ii); */
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_shutdown_switch(void)
{
    sx_status_t    status;
    sxd_status_t   sxd_status;
    int            system_err;
    sx_router_id_t vrid;
    uint32_t       data;

    SX_LOG_ENTER();

    SX_LOG_NTC("Shutdown switch\n");

    if (g_sai_db_ptr->is_bfd_module_initialized) {
        status = sx_api_bfd_deinit_set(gh_sdk);
        if (SX_ERR(status)) {
            SX_LOG_ERR("Deinit BFD module failed\n");
        }
    }

    if (SX_STATUS_SUCCESS != (status = switch_close_traps())) {
        SX_LOG_ERR("Close traps failed\n");
    }

    event_thread_asked_to_stop = true;

#ifndef _WIN32
    pthread_join(event_thread.osd.id, NULL);
#endif

    if (SAI_STATUS_SUCCESS != (status = sai_fx_uninitialize())) {
        SX_LOG_ERR("FX deinit failed.\n");
    }

    if (SAI_STATUS_SUCCESS ==
        mlnx_object_to_type(g_sai_db_ptr->default_vrid, SAI_OBJECT_TYPE_VIRTUAL_ROUTER, &data, NULL)) {
        vrid = (sx_router_id_t)data;

        if (SX_STATUS_SUCCESS != (status = sx_api_router_set(gh_sdk, SX_ACCESS_CMD_DELETE, NULL, &vrid))) {
            SX_LOG_ERR("Failed to delete default router - %s.\n", SX_STATUS_MSG(status));
        }
    }

    if (SAI_STATUS_SUCCESS != (status = mlnx_acl_deinit())) {
        SX_LOG_ERR("ACL DB deinit failed.\n");
    }

    sai_qos_db_unload(true);
    sai_buffer_db_unload(true);
    sai_acl_db_unload(true);
    sai_tunnel_db_unload(true);
    sai_db_unload(true);

    if (SX_STATUS_SUCCESS != (status = sx_api_router_deinit_set(gh_sdk))) {
        SX_LOG_ERR("Router deinit failed.\n");
    }

    if (SXD_STATUS_SUCCESS != (sxd_status = sxd_access_reg_deinit())) {
        SX_LOG_ERR("Access reg deinit failed.\n");
    }

    if (SXD_STATUS_SUCCESS != (sxd_status = sxd_dpt_deinit())) {
        SX_LOG_ERR("DPT deinit failed.\n");
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_close(&gh_sdk))) {
        SX_LOG_ERR("API close failed.\n");
    }

    memset(&g_notification_callbacks, 0, sizeof(g_notification_callbacks));
#ifdef SDK_VALGRIND
    system_err = system("killall -w memcheck-amd64-");
#else
    system_err = system("killall -w sx_sdk");
#endif

    if (0 != system_err) {
#ifdef SDK_VALGRIND
        MLNX_SAI_LOG_ERR("killall -w memcheck-amd64- failed.\n");
#else
        MLNX_SAI_LOG_ERR("killall -w sx_sdk failed.\n");
#endif
    }

#if (!defined ACS_OS) || (defined ACS_OS_NO_DOCKERS)
    system_err = system("/etc/init.d/sxdkernel stop");
    if (0 != system_err) {
        MLNX_SAI_LOG_ERR("Failed running sxdkernel stop.\n");
    }
#endif

    SX_LOG_EXIT();

    return sdk_to_sai(status);
}

static sai_status_t mlnx_disconnect_switch(void)
{
    sx_status_t status;

    SX_LOG_NTC("Disconnect switch\n");

    if (SXD_STATUS_SUCCESS != sxd_access_reg_deinit()) {
        SX_LOG_ERR("Access reg deinit failed.\n");
    }

    if (SX_STATUS_SUCCESS != (status = sx_api_close(&gh_sdk))) {
        SX_LOG_ERR("API close failed.\n");
    }

    mlnx_bmtor_fx_handle_deinit();

    memset(&g_notification_callbacks, 0, sizeof(g_notification_callbacks));

    mlnx_acl_disconnect();

    if (g_sai_qos_db_ptr != NULL) {
        free(g_sai_qos_db_ptr);
    }
    g_sai_qos_db_ptr = NULL;

    if (g_sai_acl_db_ptr != NULL) {
        free(g_sai_acl_db_ptr);
    }
    g_sai_acl_db_ptr = NULL;

    return sdk_to_sai(status);
}

/**
 * @brief Set switch attribute value
 *
 * @param[in] switch_id Switch id
 * @param[in] attr Switch attribute
 *
 * @return #SAI_STATUS_SUCCESS on success Failure status code on error
 */
static sai_status_t mlnx_set_switch_attribute(_In_ sai_object_id_t switch_id, _In_ const sai_attribute_t *attr)
{
    const sai_object_key_t key = { .key.object_id = switch_id };
    char                   key_str[MAX_KEY_STR_LEN];
    sai_status_t           sai_status;

    SX_LOG_ENTER();
    switch_key_to_str(switch_id, key_str);
    sai_status = sai_set_attribute(&key, key_str, SAI_OBJECT_TYPE_SWITCH, switch_vendor_attribs, attr);
    SX_LOG_EXIT();
    return sai_status;
}

/* Switching mode [sai_switch_switching_mode_t]
 *  (default to SAI_SWITCHING_MODE_STORE_AND_FORWARD) */
static sai_status_t mlnx_switch_mode_set(_In_ const sai_object_key_t      *key,
                                         _In_ const sai_attribute_value_t *value,
                                         void                             *arg)
{
    mlnx_port_config_t       *port;
    uint32_t                  ii;
    sx_port_forwarding_mode_t mode;
    sai_status_t              status = SAI_STATUS_SUCCESS;

    SX_LOG_ENTER();

    memset(&mode, 0, sizeof(mode));

    switch (value->s32) {
    case SAI_SWITCH_SWITCHING_MODE_CUT_THROUGH:
        mode.packet_store = SX_PORT_PACKET_STORING_MODE_CUT_THROUGH;
        break;

    case SAI_SWITCH_SWITCHING_MODE_STORE_AND_FORWARD:
        mode.packet_store = SX_PORT_PACKET_STORING_MODE_STORE_AND_FORWARD;
        break;

    default:
        SX_LOG_ERR("Invalid switching mode value %d\n", value->s32);
        return SAI_STATUS_INVALID_ATTR_VALUE_0;
    }

    sai_db_write_lock();
    mlnx_port_not_in_lag_foreach(port, ii) {
        if (SX_STATUS_SUCCESS !=
            (status = sx_api_port_forwarding_mode_set(gh_sdk, port->logical, mode))) {
            SX_LOG_ERR("Failed to set forwarding mode - %s %x %u.\n", SX_STATUS_MSG(status), port->logical, ii);
            status = sdk_to_sai(status);
            goto out;
        }
    }

    g_sai_db_ptr->packet_storing_mode = mode.packet_store;

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* Dynamic FDB entry aging time in seconds [uint32_t]
 *   Zero means aging is disabled.
 *  (default to zero)
 */
static sai_status_t mlnx_switch_aging_time_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg)
{
    sai_status_t      status;
    sx_fdb_age_time_t time;

    SX_LOG_ENTER();

    if (0 == value->u32) {
        time = SX_FDB_AGE_TIME_MAX;
    } else if (SX_FDB_AGE_TIME_MIN > value->u32) {
        time = SX_FDB_AGE_TIME_MIN;
    } else if (SX_FDB_AGE_TIME_MAX < value->u32) {
        time = SX_FDB_AGE_TIME_MAX;
    } else {
        time = value->u32;
    }

    if (SX_STATUS_SUCCESS !=
        (status = sx_api_fdb_age_time_set(gh_sdk, DEFAULT_ETH_SWID, time))) {
        SX_LOG_ERR("Failed to set fdb age time - %s.\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static mlnx_fid_flood_ctrl_attr_t mlnx_switch_flood_ctrl_attr_to_fid_attr(_In_ sai_switch_attr_t attr)
{
    switch (attr) {
    case SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION:
        return MLNX_FID_FLOOD_CTRL_ATTR_UC;

    case SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION:
        return MLNX_FID_FLOOD_CTRL_ATTR_MC;

    case SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION:
        return MLNX_FID_FLOOD_CTRL_ATTR_BC;

    default:
        SX_LOG_ERR("Unexpected flood ctrl attr - %d\n", attr);
        assert(false);
        return MLNX_FID_FLOOD_CTRL_ATTR_MAX;
    }
}

static sai_status_t mlnx_switch_flood_ctrl_set(_In_ sai_packet_action_t action, _In_ mlnx_fid_flood_ctrl_attr_t attr)
{
    sai_status_t          status;
    sai_packet_action_t  *current_action;
    sai_vlan_id_t         vlan_id;
    const mlnx_vlan_db_t *vlan;
    mlnx_bridge_t        *bridge;
    uint32_t              ii;

    assert((action == SAI_PACKET_ACTION_FORWARD) || (action == SAI_PACKET_ACTION_DROP));
    assert(attr < MLNX_FID_FLOOD_CTRL_ATTR_MAX);

    current_action = &g_sai_db_ptr->flood_actions[attr];

    if (*current_action == action) {
        return SAI_STATUS_SUCCESS;
    }

    mlnx_vlan_id_foreach(vlan_id) {
        if (!mlnx_vlan_is_created(vlan_id)) {
            continue;
        }

        vlan = mlnx_vlan_db_get_vlan(vlan_id);
        assert(vlan);

        if (action == SAI_PACKET_ACTION_FORWARD) {
            status = mlnx_fid_flood_ctrl_set_forward_after_drop(vlan_id, attr, &vlan->flood_data.types[attr]);
            if (SAI_ERR(status)) {
                return status;
            }
        } else { /* SAI_PACKET_ACTION_DROP */
            status = mlnx_fid_flood_ctrl_set_drop(vlan_id, attr, &vlan->flood_data.types[attr]);
            if (SAI_ERR(status)) {
                return status;
            }
        }
    }

    mlnx_bridge_1d_foreach(bridge, ii) {
        if (action == SAI_PACKET_ACTION_FORWARD) {
            status = mlnx_fid_flood_ctrl_set_forward_after_drop(bridge->sx_bridge_id, attr,
                                                                &bridge->flood_data.types[attr]);
            if (SAI_ERR(status)) {
                SX_LOG_ERR("Failed to set forward action for sx bridge %d\n", bridge->sx_bridge_id);
                return status;
            }
        } else { /* SAI_PACKET_ACTION_DROP */
            status = mlnx_fid_flood_ctrl_set_drop(bridge->sx_bridge_id, attr, &bridge->flood_data.types[attr]);
            if (SAI_ERR(status)) {
                SX_LOG_ERR("Failed to set drop action for sx bridge %d\n", bridge->sx_bridge_id);
                return status;
            }
        }
    }

    *current_action = action;

    return SAI_STATUS_SUCCESS;
}

/** Flood control for packets with unknown destination address.
 *   [sai_packet_action_t] (default to SAI_PACKET_ACTION_FORWARD)
 */
static sai_status_t mlnx_switch_fdb_flood_ctrl_set(_In_ const sai_object_key_t      *key,
                                                   _In_ const sai_attribute_value_t *value,
                                                   void                             *arg)
{
    sai_status_t        status;
    sai_switch_attr_t   attr_id = (sai_switch_attr_t)arg;
    sai_packet_action_t action  = (sai_packet_action_t)value->s32;

    SX_LOG_ENTER();

    assert((attr_id == SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION) ||
           (attr_id == SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION) ||
           (attr_id == SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION));

    if ((action != SAI_PACKET_ACTION_FORWARD) && (action != SAI_PACKET_ACTION_DROP)) {
        SX_LOG_ERR("Invalid packet action (%d), FORWARD or DROP is supported only\n", value->s32);
        SX_LOG_EXIT();
        return SAI_STATUS_ATTR_NOT_SUPPORTED_0;
    }

    sai_db_write_lock();

    status = mlnx_switch_flood_ctrl_set(action, mlnx_switch_flood_ctrl_attr_to_fid_attr(attr_id));

    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* ECMP hashing seed  [uint32_t] */
/* Hash algorithm for all ECMP in the switch[sai_switch_hash_algo_t] */
static sai_status_t mlnx_switch_ecmp_hash_param_set(_In_ const sai_object_key_t      *key,
                                                    _In_ const sai_attribute_value_t *value,
                                                    void                             *arg)
{
    sai_status_t                       status;
    sai_switch_attr_t                  attr_id;
    sx_router_ecmp_port_hash_params_t *port_hash_param;

    SX_LOG_ENTER();

    attr_id = (long)arg;

    assert((attr_id == SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED) ||
           (attr_id == SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM) ||
           (attr_id == SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH));

    sai_db_write_lock();

    port_hash_param = &g_sai_db_ptr->port_ecmp_hash_params;

    switch (attr_id) {
    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:
        port_hash_param->seed = value->u32;
        break;

    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM:
        port_hash_param->ecmp_hash_type = (sx_router_ecmp_hash_type_t)value->s32;
        break;

    case SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH:
        port_hash_param->symmetric_hash = value->booldata;
        break;

    default:
        SX_LOG_ERR("Unexpected attr %d\n", attr_id);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

    /* Apply DB changes to ports */
    status = mlnx_hash_ecmp_sx_config_update();
    if (SAI_ERR(status)) {
        goto out;
    }

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* The SDK can
 * 1 - Read the counters directly from HW (or)
 * 2 - Cache the counters in SW. Caching is typically done if
 * retrieval of counters directly from HW for each counter
 * read is CPU intensive
 * This setting can be used to
 * 1 - Move from HW based to SW based or Vice versa
 * 2 - Configure the SW counter cache refresh rate
 * Setting a value of 0 enables direct HW based counter read. A
 * non zero value enables the SW cache based and the counter
 * refresh rate.
 * A NPU may support both or one of the option. It would return
 * error for unsupported options. [uint32_t]
 */
static sai_status_t mlnx_switch_counter_refresh_set(_In_ const sai_object_key_t      *key,
                                                    _In_ const sai_attribute_value_t *value,
                                                    void                             *arg)
{
    SX_LOG_ENTER();

    /* TODO : implement */

    SX_LOG_EXIT();
    return SAI_STATUS_NOT_IMPLEMENTED;
}

/* Set LAG hashing seed  [uint32_t] */
static sai_status_t mlnx_switch_lag_hash_attr_set(_In_ const sai_object_key_t      *key,
                                                  _In_ const sai_attribute_value_t *value,
                                                  void                             *arg)
{
    sai_status_t               status;
    sai_switch_attr_t          attr_id;
    sx_lag_port_hash_params_t *lag_hash_params;

    SX_LOG_ENTER();

    attr_id = (long)arg;

    assert((attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED) ||
           (attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM) ||
           (attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH));

    sai_db_write_lock();

    lag_hash_params = &g_sai_db_ptr->lag_hash_params;

    switch (attr_id) {
    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:
        lag_hash_params->lag_seed = value->u32;
        break;

    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM:
        lag_hash_params->lag_hash_type = (sx_lag_hash_type_t)value->s32;
        break;

    case SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH:
        lag_hash_params->is_lag_hash_symmetric = value->booldata;
        break;

    default:
        SX_LOG_ERR("Unexpected attr %d\n", attr_id);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

    status = mlnx_hash_lag_sx_config_update();
    if (SAI_ERR(status)) {
        goto out;
    }

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/**
 * @brief Get switch attribute value
 *
 * @param[in] switch_id Switch id
 * @param[in] attr_count Number of attributes
 * @param[inout] attr_list Array of switch attributes
 *
 * @return #SAI_STATUS_SUCCESS on success Failure status code on error
 */
static sai_status_t mlnx_get_switch_attribute(_In_ sai_object_id_t     switch_id,
                                              _In_ sai_uint32_t        attr_count,
                                              _Inout_ sai_attribute_t *attr_list)
{
    sai_status_t           status;
    const sai_object_key_t key = { .key.object_id = switch_id };
    char                   key_str[MAX_KEY_STR_LEN];

    SX_LOG_ENTER();
    switch_key_to_str(switch_id, key_str);
    status = sai_get_attributes(&key, key_str, SAI_OBJECT_TYPE_SWITCH, switch_vendor_attribs, attr_count, attr_list);
    SX_LOG_EXIT();
    return status;
}

/* The number of ports on the switch [uint32_t] */
static sai_status_t mlnx_switch_port_number_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg)
{
    SX_LOG_ENTER();

    cl_plock_acquire(&g_sai_db_ptr->p_lock);
    value->u32 = g_sai_db_ptr->ports_number;
    cl_plock_release(&g_sai_db_ptr->p_lock);

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_max_ports_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = MAX_PORTS;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Get the port list [sai_object_list_t] */
static sai_status_t mlnx_switch_port_list_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    sai_object_id_t    *ports = NULL;
    sai_status_t        status;
    mlnx_port_config_t *port;
    uint32_t            ii, jj = 0;

    SX_LOG_ENTER();

    ports = calloc(MAX_PORTS, sizeof(*ports));
    if (!ports) {
        SX_LOG_ERR("Failed to allocate memory\n");
        return SAI_STATUS_NO_MEMORY;
    }

    sai_db_write_lock();

    mlnx_port_phy_foreach(port, ii) {
        ports[jj++] = port->saiport;
    }

    status = mlnx_fill_objlist(ports, g_sai_db_ptr->ports_number, &value->objlist);

    sai_db_unlock();

    free(ports);
    SX_LOG_EXIT();
    return status;
}

/* Get the Max MTU in bytes, Supported by the switch [uint32_t] */
static sai_status_t mlnx_switch_max_mtu_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.port_mtu_max;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Get the CPU Port [sai_object_id_t] */
static sai_status_t mlnx_switch_cpu_port_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg)
{
    sai_status_t status;

    SX_LOG_ENTER();

    if (SAI_STATUS_SUCCESS != (status = mlnx_create_object(SAI_OBJECT_TYPE_PORT, CPU_PORT, NULL, &value->oid))) {
        return status;
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Max number of virtual routers supported [uint32_t] */
static sai_status_t mlnx_switch_max_vr_get(_In_ const sai_object_key_t   *key,
                                           _Inout_ sai_attribute_value_t *value,
                                           _In_ uint32_t                  attr_index,
                                           _Inout_ vendor_cache_t        *cache,
                                           void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.router_vrid_max;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The FDB Table size [uint32_t] */
static sai_status_t mlnx_switch_fdb_size_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_fdb_table_size;
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The L3 Host Table size [uint32_t] */
static sai_status_t mlnx_switch_neighbor_size_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_neighbor_table_size;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The L3 Route Table size [uint32_t] */
static sai_status_t mlnx_switch_route_size_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_route_table_size;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/*
 *   Local subnet routing supported [bool]
 *   Routes with next hop set to "on-link"
 */
static sai_status_t mlnx_switch_on_link_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg)
{
    SX_LOG_ENTER();

    value->booldata = true;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Oper state [sai_switch_oper_status_t] */
static sai_status_t mlnx_switch_oper_status_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg)
{
    SX_LOG_ENTER();

    value->s32 = SAI_SWITCH_OPER_STATUS_UP;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The current value of the maximum temperature
 * retrieved from the switch sensors, in Celsius [int32_t] */
static sai_status_t mlnx_switch_max_temp_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg)
{
    struct ku_mtmp_reg tmp_reg;
    sxd_reg_meta_t     reg_meta;
    int16_t            tmp = 0;
    sxd_status_t       sxd_status;

    #define TEMP_MASUREMENT_UNIT 0.125

    SX_LOG_ENTER();

    memset(&tmp_reg, 0, sizeof(tmp_reg));
    memset(&reg_meta, 0, sizeof(reg_meta));
    tmp_reg.sensor_index = 0;
    reg_meta.access_cmd  = SXD_ACCESS_CMD_GET;
    reg_meta.dev_id      = SX_DEVICE_ID;
    reg_meta.swid        = DEFAULT_ETH_SWID;

    sxd_status = sxd_access_reg_mtmp(&tmp_reg, &reg_meta, 1, NULL, NULL);
    if (sxd_status) {
        SX_LOG_ERR("Access_mtmp_reg failed with status (%s:%d)\n", SXD_STATUS_MSG(sxd_status), sxd_status);
        return SAI_STATUS_FAILURE;
    }
    if (((int16_t)tmp_reg.temperature) < 0) {
        tmp = (0xFFFF + ((int16_t)tmp_reg.temperature) + 1);
    } else {
        tmp = (int16_t)tmp_reg.temperature;
    }
    value->s32 = (int32_t)(tmp * TEMP_MASUREMENT_UNIT);

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/** QoS Map Id [sai_object_id_t] */
static sai_status_t mlnx_switch_qos_map_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    sai_qos_map_type_t qos_map_type = (sai_qos_map_type_t)arg;
    uint32_t           qos_map_id;

    assert(qos_map_type < MLNX_QOS_MAP_TYPES_MAX);

    sai_db_read_lock();

    qos_map_id = g_sai_db_ptr->switch_qos_maps[qos_map_type];

    if (!qos_map_id) {
        value->oid = SAI_NULL_OBJECT_ID;
        sai_db_unlock();
        return SAI_STATUS_SUCCESS;
    }

    sai_db_unlock();
    return mlnx_create_object(SAI_OBJECT_TYPE_QOS_MAP, qos_map_id, NULL, &value->oid);
}

/** QoS Map Id [sai_object_id_t] */
static sai_status_t mlnx_switch_qos_map_id_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg)
{
    sai_qos_map_type_t  qos_map_type = (sai_qos_map_type_t)arg;
    sai_status_t        status       = SAI_STATUS_SUCCESS;
    mlnx_port_config_t *port;
    uint32_t            qos_map_id;
    uint32_t            port_idx;

    assert(qos_map_type < MLNX_QOS_MAP_TYPES_MAX);

    if (value->oid == SAI_NULL_OBJECT_ID) {
        qos_map_id = 0;
    } else {
        status = mlnx_object_to_type(value->oid, SAI_OBJECT_TYPE_QOS_MAP, &qos_map_id, NULL);
        if (status != SAI_STATUS_SUCCESS) {
            return status;
        }
    }

    sai_db_write_lock();

    mlnx_port_not_in_lag_foreach(port, port_idx) {
        if (port->qos_maps[qos_map_type]) {
            continue;
        }

        status = mlnx_port_qos_map_apply(port->saiport, value->oid, qos_map_type);
        if (status != SAI_STATUS_SUCCESS) {
            SX_LOG_ERR("Failed to update port %" PRIx64 " with QoS map %" PRIx64 "\n",
                       port->saiport, value->oid);
            break;
        }

        SX_LOG_NTC("Port %" PRIx64 " was updated with new QoS map %" PRIx64 "\n",
                   port->saiport, value->oid);
    }

    g_sai_db_ptr->switch_qos_maps[qos_map_type] = qos_map_id;

    sai_db_sync();
    sai_db_unlock();
    return status;
}

/** Default Traffic class Mapping [sai_uint8_t], Default TC=0*/
static sai_status_t mlnx_switch_default_tc_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    SX_LOG_ENTER();

    sai_db_read_lock();
    value->u8 = g_sai_db_ptr->switch_default_tc;
    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/** Default Traffic class Mapping [sai_uint8_t], Default TC=0*/
static sai_status_t mlnx_switch_default_tc_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg)
{
    mlnx_port_config_t *port;
    uint32_t            port_idx;
    sai_status_t        status;

    SX_LOG_ENTER();

    if (!SX_CHECK_MAX(value->u8, SXD_COS_PORT_PRIO_MAX)) {
        SX_LOG_ERR("Invalid tc(%u)\n", value->u8);
        return SAI_STATUS_INVALID_PARAMETER;
    }

    sai_db_write_lock();
    g_sai_db_ptr->switch_default_tc = value->u8;
    sai_db_unlock();

    mlnx_port_not_in_lag_foreach(port, port_idx) {
        if (port->default_tc) {
            continue;
        }

        status = mlnx_port_tc_set(port->logical, value->u8);
        if (status != SAI_STATUS_SUCCESS) {
            SX_LOG_ERR("Failed to update port %" PRIx64 " with tc(%u)\n", port->saiport, value->u8);
            break;
        }

        SX_LOG_NTC("Port %" PRIx64 " was updated with tc(%u)\n", port->saiport, value->u8);
    }

    SX_LOG_EXIT();
    return status;
}

/* minimum priority for ACL table [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_table_min_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = ACL_MIN_TABLE_PRIO;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* maximum priority for ACL table [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_table_max_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = ACL_MAX_TABLE_PRIO;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* minimum priority for ACL entry [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_entry_min_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = mlnx_acl_entry_min_prio_get();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* maximum priority for ACL entry [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_entry_max_prio_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = mlnx_acl_entry_max_prio_get();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* minimum priority for ACL Table Group entry [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_table_group_min_prio_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = ACL_GROUP_MEMBER_PRIO_MIN;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* maximum priority for ACL Table Group entry [sai_uint32_t] */
static sai_status_t mlnx_switch_acl_table_group_max_prio_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = ACL_GROUP_MEMBER_PRIO_MAX;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* ACL capability */
static sai_status_t mlnx_switch_acl_capability_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg)
{
    sai_acl_stage_t stage;

    stage = (int64_t)arg;

    value->aclcapability.is_action_list_mandatory = false;

    return mlnx_acl_stage_action_types_list_get(stage, &value->aclcapability.action_list);
}

/* Count of the total number of actions supported by NPU */
static sai_status_t mlnx_switch_max_acl_action_count_get(_In_ const sai_object_key_t   *key,
                                                         _Inout_ sai_attribute_value_t *value,
                                                         _In_ uint32_t                  attr_index,
                                                         _Inout_ vendor_cache_t        *cache,
                                                         void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = mlnx_acl_action_types_count_get();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_max_acl_range_count_get(_In_ const sai_object_key_t   *key,
                                                        _Inout_ sai_attribute_value_t *value,
                                                        _In_ uint32_t                  attr_index,
                                                        _Inout_ vendor_cache_t        *cache,
                                                        void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.acl_port_ranges_max;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* ACL user-based trap id range [sai_u32_range_t] */
static sai_status_t mlnx_switch_acl_trap_range_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg)
{
    SX_LOG_ENTER();

    value->u32range.min = SX_TRAP_ID_ACL_MIN;
    value->u32range.max = SX_TRAP_ID_ACL_MAX;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* ACL user-based ACL meta data range [sai_u32_range_t] */
static sai_status_t mlnx_switch_acl_meta_range_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg)
{
    SX_LOG_ENTER();

    value->u32range.min = ACL_USER_META_RANGE_MIN;
    value->u32range.max = ACL_USER_META_RANGE_MAX;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Maximum number of ports that can be part of a LAG [uint32_t] */
static sai_status_t mlnx_switch_max_lag_members_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.lag_port_members_max;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Maximum number of LAGs that can be created per switch [uint32_t] */
static sai_status_t mlnx_switch_max_lag_number_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.lag_num_max;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Switching mode [sai_switch_switching_mode_t]
 *  (default to SAI_SWITCHING_MODE_STORE_AND_FORWARD) */
static sai_status_t mlnx_switch_mode_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg)
{
    sai_status_t                  status = SAI_STATUS_SUCCESS;
    sx_port_packet_storing_mode_t packet_storing_mode;

    SX_LOG_ENTER();

    sai_db_read_lock();

    packet_storing_mode = g_sai_db_ptr->packet_storing_mode;

    switch (packet_storing_mode) {
    case SX_PORT_PACKET_STORING_MODE_CUT_THROUGH:
        value->s32 = SAI_SWITCH_SWITCHING_MODE_CUT_THROUGH;
        break;

    case SX_PORT_PACKET_STORING_MODE_STORE_AND_FORWARD:
        value->s32 = SAI_SWITCH_SWITCHING_MODE_STORE_AND_FORWARD;
        break;

    default:
        SX_LOG_ERR("Unexpected forwarding mode %u\n", packet_storing_mode);
        status = SAI_STATUS_FAILURE;
        goto out;
    }

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

sai_status_t mlnx_switch_get_mac(sx_mac_addr_t *mac)
{
    mlnx_port_config_t *first_port = NULL;
    mlnx_port_config_t *port       = NULL;
    uint32_t            port_idx;
    sai_status_t        status;

    mlnx_port_phy_foreach(port, port_idx) {
        first_port = port;
        break;
    }

    if (!first_port) {
        SX_LOG_ERR("Failed to get switch mac - first port not found\n");
        return SAI_STATUS_FAILURE;
    }

    /* Use switch first port, and zero down lower 6 bits port part (64 ports) */
    status = sx_api_port_phys_addr_get(gh_sdk, first_port->logical, mac);
    if (SX_ERR(status)) {
        SX_LOG_ERR("Failed to get port %x address - %s.\n", first_port->logical, SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }
    mac->ether_addr_octet[5] &= mlnx_port_mac_mask_get();

    return SAI_STATUS_SUCCESS;
}

/* Default switch MAC Address [sai_mac_t] */
static sai_status_t mlnx_switch_src_mac_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg)
{
    sai_status_t  status;
    sx_mac_addr_t mac;

    SX_LOG_ENTER();

    sai_db_read_lock();

    status = mlnx_switch_get_mac(&mac);
    if (SAI_ERR(status)) {
        goto out;
    }

    memcpy(value->mac, &mac,  sizeof(value->mac));

out:
    sai_db_unlock();
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

/* Dynamic FDB entry aging time in seconds [uint32_t]
 *   Zero means aging is disabled.
 *  (default to zero)
 */
static sai_status_t mlnx_switch_aging_time_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    sai_status_t      status;
    sx_fdb_age_time_t age_time;

    SX_LOG_ENTER();

    if (SX_STATUS_SUCCESS != (status = sx_api_fdb_age_time_get(gh_sdk, DEFAULT_ETH_SWID, &age_time))) {
        SX_LOG_ERR("Failed to get fdb age time - %s.\n", SX_STATUS_MSG(status));
        return sdk_to_sai(status);
    }

    value->u32 = age_time;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/** Flood control for packets with unknown destination address.
 *   [sai_packet_action_t] (default to SAI_PACKET_ACTION_FORWARD)
 */
static sai_status_t mlnx_switch_fdb_flood_ctrl_get(_In_ const sai_object_key_t   *key,
                                                   _Inout_ sai_attribute_value_t *value,
                                                   _In_ uint32_t                  attr_index,
                                                   _Inout_ vendor_cache_t        *cache,
                                                   void                          *arg)
{
    sai_switch_attr_t          attr_id = (sai_switch_attr_t)arg;
    mlnx_fid_flood_ctrl_attr_t flood_ctrl_attr;

    SX_LOG_ENTER();

    assert((attr_id == SAI_SWITCH_ATTR_FDB_UNICAST_MISS_PACKET_ACTION) ||
           (attr_id == SAI_SWITCH_ATTR_FDB_MULTICAST_MISS_PACKET_ACTION) ||
           (attr_id == SAI_SWITCH_ATTR_FDB_BROADCAST_MISS_PACKET_ACTION));

    sai_db_read_lock();

    flood_ctrl_attr = mlnx_switch_flood_ctrl_attr_to_fid_attr(attr_id);

    value->s32 = g_sai_db_ptr->flood_actions[flood_ctrl_attr];

    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* ECMP hashing seed  [uint32_t] */
/* Hash algorithm for all ECMP in the switch[sai_switch_hash_algo_t] */
static sai_status_t mlnx_switch_ecmp_hash_param_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg)
{
    const sx_router_ecmp_port_hash_params_t *port_hash_param;
    sai_status_t                             status = SAI_STATUS_SUCCESS;

    assert(((long)arg == SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED) ||
           ((long)arg == SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM) ||
           ((long)arg == SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH));

    SX_LOG_ENTER();

    sai_db_read_lock();

    port_hash_param = &g_sai_db_ptr->port_ecmp_hash_params;

    switch ((long)arg) {
    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_SEED:
        value->u32 = port_hash_param->seed;
        break;

    case SAI_SWITCH_ATTR_ECMP_DEFAULT_HASH_ALGORITHM:
        value->s32 = (sai_hash_algorithm_t)port_hash_param->ecmp_hash_type;
        break;

    case SAI_SWITCH_ATTR_ECMP_DEFAULT_SYMMETRIC_HASH:
        value->booldata = port_hash_param->symmetric_hash;
        break;
    }

    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* ECMP number of members per group [sai_uint32_t] */
static sai_status_t mlnx_switch_ecmp_members_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = ECMP_MAX_PATHS;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* ECMP number of group [uint32_t] */
static sai_status_t mlnx_switch_ecmp_groups_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg)
{
    SX_LOG_ENTER();

    /* same as .kvd_linear_size = 0x10000 = 64K */
    value->u32 = 0x10000;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The SDK can
 * 1 - Read the counters directly from HW (or)
 * 2 - Cache the counters in SW. Caching is typically done if
 * retrieval of counters directly from HW for each counter
 * read is CPU intensive
 * This setting can be used to
 * 1 - Move from HW based to SW based or Vice versa
 * 2 - Configure the SW counter cache refresh rate
 * Setting a value of 0 enables direct HW based counter read. A
 * non zero value enables the SW cache based and the counter
 * refresh rate.
 * A NPU may support both or one of the option. It would return
 * error for unsupported options. [uint32_t]
 */
static sai_status_t mlnx_switch_counter_refresh_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg)
{
    SX_LOG_ENTER();

    /* TODO : implement */
    SX_LOG_EXIT();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

/* Default trap group [sai_object_id_t] */
static sai_status_t mlnx_switch_default_trap_group_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    cl_plock_acquire(&g_sai_db_ptr->p_lock);
    value->oid = g_sai_db_ptr->default_trap_group;
    cl_plock_release(&g_sai_db_ptr->p_lock);

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Default SAI Virtual Router ID [sai_object_id_t] */
static sai_status_t mlnx_switch_default_vrid_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg)
{
    SX_LOG_ENTER();

    cl_plock_acquire(&g_sai_db_ptr->p_lock);
    value->oid = g_sai_db_ptr->default_vrid;
    cl_plock_release(&g_sai_db_ptr->p_lock);

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/** HQOS - Maximum Number of Hierarchy scheduler
 *  group levels(depth) supported [sai_uint32_t]*/
static sai_status_t mlnx_switch_sched_group_levels_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = MAX_SCHED_LEVELS;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/** HQOS - Maximum number of scheduler groups supported on
 * each Hierarchy level [sai_u32_list_t] */
static sai_status_t mlnx_switch_sched_groups_count_per_level_get(_In_ const sai_object_key_t   *key,
                                                                 _Inout_ sai_attribute_value_t *value,
                                                                 _In_ uint32_t                  attr_index,
                                                                 _Inout_ vendor_cache_t        *cache,
                                                                 void                          *arg)
{
    uint32_t    *groups_count = NULL;
    sai_status_t status;
    uint32_t     ii;

    SX_LOG_ENTER();

    groups_count = malloc(MAX_SCHED_LEVELS * sizeof(uint32_t));
    if (!groups_count) {
        SX_LOG_ERR("Failed to max groups count list per level\n");
        return SAI_STATUS_NO_MEMORY;
    }

    for (ii = 0; ii < MAX_SCHED_LEVELS; ii++) {
        groups_count[ii] = MAX_SCHED_CHILD_GROUPS;
    }

    status = mlnx_fill_u32list(groups_count, MAX_SCHED_LEVELS, &value->u32list);

    SX_LOG_EXIT();
    free(groups_count);
    return status;
}

/** HQOS - Maximum number of childs supported per scheudler group [sai_uint32_t]*/
static sai_status_t mlnx_switch_sched_max_child_groups_count_get(_In_ const sai_object_key_t   *key,
                                                                 _Inout_ sai_attribute_value_t *value,
                                                                 _In_ uint32_t                  attr_index,
                                                                 _Inout_ vendor_cache_t        *cache,
                                                                 void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = MAX_SCHED_CHILD_GROUPS;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The number of Unicast Queues per port [sai_uint32_t]
 * The number of Multicast Queues per port [sai_uint32_t]
 * The total number of Queues per port [sai_uint32_t]
 * The number of lossless queues per port supported by the switch [sai_uint32_t] */
static sai_status_t mlnx_switch_queue_num_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    long attr = (long)arg;

    SX_LOG_ENTER();

    assert((SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES == attr) ||
           (SAI_SWITCH_ATTR_NUMBER_OF_MULTICAST_QUEUES == attr) ||
           (SAI_SWITCH_ATTR_NUMBER_OF_QUEUES == attr) ||
           (SAI_SWITCH_ATTR_QOS_NUM_LOSSLESS_QUEUES == attr));

    switch (attr) {
    case SAI_SWITCH_ATTR_NUMBER_OF_UNICAST_QUEUES:
        value->u32 = (g_resource_limits.cos_port_ets_traffic_class_max + 1) / 2;
        break;

    case SAI_SWITCH_ATTR_NUMBER_OF_MULTICAST_QUEUES:
        value->u32 = (g_resource_limits.cos_port_ets_traffic_class_max + 1) / 2;
        break;

    case SAI_SWITCH_ATTR_NUMBER_OF_QUEUES:
    case SAI_SWITCH_ATTR_QOS_NUM_LOSSLESS_QUEUES:
        value->u32 = g_resource_limits.cos_port_ets_traffic_class_max + 1;
        break;
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The ECMP hash object ID [sai_object_id_t] */
/* The LAG hash object ID [sai_object_id_t] */
static sai_status_t mlnx_switch_hash_object_get(_In_ const sai_object_key_t   *key,
                                                _Inout_ sai_attribute_value_t *value,
                                                _In_ uint32_t                  attr_index,
                                                _Inout_ vendor_cache_t        *cache,
                                                void                          *arg)
{
    long     attr    = (long)arg;
    uint32_t hash_id = 0;

    SX_LOG_ENTER();

    switch (attr) {
    case SAI_SWITCH_ATTR_ECMP_HASH:
        hash_id = SAI_HASH_ECMP_ID;
        break;

    case SAI_SWITCH_ATTR_ECMP_HASH_IPV4:
        hash_id = SAI_HASH_ECMP_IP4_ID;
        break;

    case SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4:
        hash_id = SAI_HASH_ECMP_IPINIP_ID;
        break;

    case SAI_SWITCH_ATTR_ECMP_HASH_IPV6:
        hash_id = SAI_HASH_ECMP_IP6_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH:
        hash_id = SAI_HASH_LAG_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV4:
        hash_id = SAI_HASH_LAG_IP4_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4:
        hash_id = SAI_HASH_LAG_IPINIP_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV6:
        hash_id = SAI_HASH_LAG_IP6_ID;
        break;

    default:
        /* Should not reach this */
        assert(false);
        break;
    }

    sai_db_read_lock();
    value->oid = g_sai_db_ptr->oper_hash_list[hash_id];
    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* The ECMP hash object ID [sai_object_id_t] */
/* The LAG hash object ID [sai_object_id_t] */
static sai_status_t mlnx_switch_hash_object_set(_In_ const sai_object_key_t      *key,
                                                _In_ const sai_attribute_value_t *value,
                                                void                             *arg)
{
    long                               attr        = (long)arg;
    sai_object_id_t                    hash_obj_id = value->oid;
    sai_object_id_t                    old_hash_obj_id;
    mlnx_switch_usage_hash_object_id_t hash_oper_id;
    sai_status_t                       status = SAI_STATUS_SUCCESS;
    bool                               is_applicable;

    SX_LOG_ENTER();

    switch (attr) {
    case SAI_SWITCH_ATTR_ECMP_HASH_IPV4_IN_IPV4:
        hash_oper_id = SAI_HASH_ECMP_IPINIP_ID;
        break;

    case SAI_SWITCH_ATTR_ECMP_HASH_IPV4:
        hash_oper_id = SAI_HASH_ECMP_IP4_ID;
        break;

    case SAI_SWITCH_ATTR_ECMP_HASH_IPV6:
        hash_oper_id = SAI_HASH_ECMP_IP6_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV4:
        hash_oper_id = SAI_HASH_LAG_IP4_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV4_IN_IPV4:
        hash_oper_id = SAI_HASH_LAG_IPINIP_ID;
        break;

    case SAI_SWITCH_ATTR_LAG_HASH_IPV6:
        hash_oper_id = SAI_HASH_LAG_IP6_ID;
        break;

    default:
        SX_LOG_ERR("Unexpected attr - %ld\n", attr);
        return SAI_STATUS_FAILURE;
    }

    sai_db_write_lock();

    if (g_sai_db_ptr->oper_hash_list[hash_oper_id] == hash_obj_id) {
        status = SAI_STATUS_SUCCESS;
        goto out;
    }

    if (hash_obj_id != SAI_NULL_OBJECT_ID) {
        status = mlnx_hash_object_is_applicable(hash_obj_id, hash_oper_id, &is_applicable);
        if (SAI_ERR(status)) {
            goto out;
        }

        if (!is_applicable) {
            status = SAI_STATUS_NOT_SUPPORTED;
            goto out;
        }
    }

    old_hash_obj_id                            = g_sai_db_ptr->oper_hash_list[hash_oper_id];
    g_sai_db_ptr->oper_hash_list[hash_oper_id] = value->oid;

    status = mlnx_hash_config_db_changes_commit(hash_oper_id);
    if (SAI_ERR(status)) {
        g_sai_db_ptr->oper_hash_list[hash_oper_id] = old_hash_obj_id;
        goto out;
    }

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* restart warm [bool] */
static sai_status_t mlnx_switch_restart_warm_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg)
{
    SX_LOG_ENTER();

    sai_db_read_lock();
    value->booldata = g_sai_db_ptr->restart_warm;
    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* restart warm [bool] */
static sai_status_t mlnx_switch_warm_recover_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg)
{
    SX_LOG_ENTER();

    sai_db_read_lock();
    value->booldata = g_sai_db_ptr->warm_recover;
    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* This function needs to be guarded by write lock */
static sai_status_t mlnx_switch_restart_warm()
{
    sx_host_ifc_register_key_t reg;
    sx_status_t                sx_status  = SX_STATUS_ERROR;
    sai_status_t               sai_status = SAI_STATUS_FAILURE;
    uint32_t                   ii;
    sx_fd_t                    sx_callback_channel_fd;
    sx_issu_pause_t            sx_issu_pause_param;
    sxd_status_t               sxd_status;

    SX_LOG_ENTER();

    memset(&reg, 0, sizeof(reg));
    memcpy(&sx_callback_channel_fd, &g_sai_db_ptr->callback_channel.channel.fd, sizeof(sx_callback_channel_fd));

    reg.key_type = SX_HOST_IFC_REGISTER_KEY_TYPE_GLOBAL;
    for (ii = 0; END_TRAP_INFO_ID != mlnx_traps_info[ii].trap_id; ii++) {
        if (0 == mlnx_traps_info[ii].sdk_traps_num) {
            continue;
        }
        sai_status = mlnx_register_trap(SX_ACCESS_CMD_DEREGISTER, ii,
                                        SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_CB,
                                        sx_callback_channel_fd, &reg);
        if (SAI_ERR(sai_status)) {
            SX_LOG_ERR("Error deregistering trap #%d\n", ii);
            goto out;
        }
    }

    sai_status = mlnx_acl_psort_thread_suspend();
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Error suspending acl psort thread\n");
        goto out;
    }

    sai_db_unlock();
    event_thread_asked_to_stop = true;
#ifndef _WIN32
    pthread_join(event_thread.osd.id, NULL);
#endif
    sai_db_write_lock();

    if (SXD_STATUS_SUCCESS != (sxd_status = sxd_access_reg_deinit())) {
        SX_LOG_ERR("Access reg deinit failed.\n");
        sai_status = SAI_STATUS_FAILURE;
        goto out;
    }

    memset(&sx_issu_pause_param, 0, sizeof(sx_issu_pause_param));
    sx_status = sx_api_issu_pause_set(gh_sdk, &sx_issu_pause_param);
    if (SX_STATUS_SUCCESS != sx_status) {
        SX_LOG_ERR("Error pausing sdk: %s\n", SX_STATUS_MSG(sx_status));
        sai_status = sdk_to_sai(sx_status);
        goto out;
    }

    sx_status = sx_api_close(&gh_sdk);
    if (SX_STATUS_SUCCESS != sx_status) {
        SX_LOG_ERR("Error closing sdk handle: %s\n", SX_STATUS_MSG(sx_status));
        sai_status = sdk_to_sai(sx_status);
        goto out;
    }

#ifdef CONFIG_SYSLOG
    closelog();
    g_log_init = false;
#endif

    sai_status = SAI_STATUS_SUCCESS;

out:
    SX_LOG_EXIT();
    return sai_status;
}

/* This function needs to be guarded by write lock */
static sai_status_t mlnx_switch_warm_recover(_In_ sai_object_id_t switch_id)
{
    sx_host_ifc_register_key_t reg;
    sx_status_t                sx_status  = SX_STATUS_ERROR;
    sai_status_t               sai_status = SAI_STATUS_FAILURE;
    uint32_t                   ii;
    sx_fd_t                    sx_callback_channel_fd;
    cl_status_t                cl_err;
    sx_issu_resume_t           sx_issu_resume_param;
    const bool                 warm_recover = true;
    mlnx_sai_boot_type_t       boot_type;

    SX_LOG_ENTER();

    if (SX_STATUS_SUCCESS != (sx_status = sx_api_open(sai_log_cb, &gh_sdk))) {
        MLNX_SAI_LOG_ERR("Can't open connection to SDK - %s.\n", SX_STATUS_MSG(sx_status));
        sai_status = sdk_to_sai(sx_status);
        goto out;
    }

    memset(&sx_issu_resume_param, 0, sizeof(sx_issu_resume_param));
    sx_status = sx_api_issu_resume_set(gh_sdk, &sx_issu_resume_param);
    if (SX_STATUS_SUCCESS != sx_status) {
        SX_LOG_ERR("Error resuming sdk: %s\n", SX_STATUS_MSG(sx_status));
        sai_status = sdk_to_sai(sx_status);
        goto out;
    }

    boot_type = g_sai_db_ptr->boot_type;

    sai_db_unlock();
    sai_status = mlnx_resource_mng_stage(warm_recover, boot_type);
    if (SAI_STATUS_SUCCESS != sai_status) {
        SX_LOG_ERR("Error in resource mng stage\n");
        sai_db_write_lock();
        goto out;
    }
    sai_db_write_lock();

    memset(&reg, 0, sizeof(reg));

    sx_status = sx_api_host_ifc_open(gh_sdk, &g_sai_db_ptr->callback_channel.channel.fd);
    if (SX_STATUS_SUCCESS != sx_status) {
        SX_LOG_ERR("Error opening host ifc for fd: %s", SX_STATUS_MSG(sx_status));
        goto out;
    }

    memcpy(&sx_callback_channel_fd, &g_sai_db_ptr->callback_channel.channel.fd, sizeof(sx_callback_channel_fd));
    reg.key_type = SX_HOST_IFC_REGISTER_KEY_TYPE_GLOBAL;
    for (ii = 0; END_TRAP_INFO_ID != mlnx_traps_info[ii].trap_id; ii++) {
        if (0 == mlnx_traps_info[ii].sdk_traps_num) {
            continue;
        }

        sai_status = mlnx_register_trap(SX_ACCESS_CMD_REGISTER, ii,
                                        SAI_HOSTIF_TABLE_ENTRY_CHANNEL_TYPE_CB,
                                        sx_callback_channel_fd, &reg);
        if (SAI_STATUS_SUCCESS != sai_status) {
            SX_LOG_ERR("Error registering trap #%d\n", ii);
            goto out;
        }
    }

    sai_db_unlock();
    sai_status = mlnx_netdev_restore();
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Failed to restore netdev\n");
        sai_db_write_lock();
        sai_status = SAI_STATUS_FAILURE;
        goto out;
    }

    event_thread_asked_to_stop = false;
    cl_err                     = cl_thread_init(&event_thread, event_thread_func, (const void*const)switch_id, NULL);
    if (cl_err) {
        SX_LOG_ERR("Failed to create event thread\n");
        sai_db_write_lock();
        sai_status = SAI_STATUS_FAILURE;
        goto out;
    }
    sai_db_write_lock();

    sai_status = mlnx_acl_psort_thread_resume();
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Error resuming acl psort thread\n");
        goto out;
    }

    sai_status = SAI_STATUS_SUCCESS;
out:
    SX_LOG_EXIT();
    return sai_status;
}

/* restart warm [bool] */
static sai_status_t mlnx_switch_restart_warm_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg)
{
#ifdef ACS_OS
    /* On Sonic, check point restore is disabled. This flow is used for FFB and does nothing. */
    return SAI_STATUS_SUCCESS;
#else
    sai_status_t status = SAI_STATUS_FAILURE;

    SX_LOG_ENTER();

    sai_db_write_lock();
    if (value->booldata) {
        status = mlnx_switch_restart_warm();
        if (SAI_ERR(status)) {
            MLNX_SAI_LOG_ERR("Error restarting warm\n");
            goto out;
        }
    }
    g_sai_db_ptr->restart_warm = value->booldata;
    status                     = SAI_STATUS_SUCCESS;

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
#endif
}

/* restart warm [bool] */
static sai_status_t mlnx_switch_warm_recover_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg)
{
    sai_status_t status = SAI_STATUS_FAILURE;

    SX_LOG_ENTER();

    sai_db_write_lock();
    if (value->booldata) {
        status = mlnx_switch_warm_recover(key->key.object_id);
        if (SAI_ERR(status)) {
            MLNX_SAI_LOG_ERR("Error warm recovering\n");
            goto out;
        }
    }
    g_sai_db_ptr->warm_recover = value->booldata;
    status                     = SAI_STATUS_SUCCESS;

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

/* LAG hashing seed  [uint32_t] */
static sai_status_t mlnx_switch_lag_hash_attr_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg)
{
    const sx_lag_port_hash_params_t *lag_hash_params;
    sai_switch_attr_t                attr_id;

    attr_id = (long)arg;

    assert((attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED) ||
           (attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM) ||
           (attr_id == SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH));

    SX_LOG_ENTER();

    sai_db_read_lock();

    lag_hash_params = &g_sai_db_ptr->lag_hash_params;

    switch ((long)arg) {
    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_SEED:
        value->u32 = lag_hash_params->lag_seed;
        break;

    case SAI_SWITCH_ATTR_LAG_DEFAULT_HASH_ALGORITHM:
        value->s32 = (sai_hash_algorithm_t)lag_hash_params->lag_hash_type;
        break;

    case SAI_SWITCH_ATTR_LAG_DEFAULT_SYMMETRIC_HASH:
        value->booldata = lag_hash_params->is_lag_hash_symmetric;
        break;
    }

    sai_db_unlock();
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_available_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    sx_status_t         sx_status;
    rm_sdk_table_type_e table_type;
    uint32_t            free_entries, init_limit = UINT_MAX;

    SX_LOG_ENTER();

    switch ((int64_t)arg) {
    case SAI_SWITCH_ATTR_AVAILABLE_IPV4_ROUTE_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_FIB_IPV4_UC_E;
        init_limit = g_ipv4_route_table_size;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_IPV6_ROUTE_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_FIB_IPV6_UC_E;
        init_limit = g_ipv6_route_table_size;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEXTHOP_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ADJACENCY_E;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEXTHOP_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ADJACENCY_E;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_IPV4_NEIGHBOR_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ARP_IPV4_E;
        init_limit = g_ipv4_neighbor_table_size;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_IPV6_NEIGHBOR_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ARP_IPV6_E;
        init_limit = g_ipv6_neighbor_table_size;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ADJACENCY_E;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_NEXT_HOP_GROUP_MEMBER_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_ADJACENCY_E;
        break;

    case SAI_SWITCH_ATTR_AVAILABLE_FDB_ENTRY:
        table_type = RM_SDK_TABLE_TYPE_UC_MAC_E;
        break;

    default:
        SX_LOG_ERR("Unexpected type of arg (%ld)\n", (int64_t)arg);
        assert(false);
    }

    sx_status = sx_api_rm_free_entries_by_type_get(gh_sdk, table_type, &free_entries);
    if (SX_ERR(sx_status)) {
        SX_LOG_ERR("Failed to get a number of free resources for sx table %d - %s\n",
                   table_type, SX_STATUS_MSG(sx_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sx_status);
    }

    value->u32 = free_entries;
    if (init_limit > 0) {
        value->u32 = MIN(free_entries, init_limit);
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_acl_available_get(_In_ const sai_object_key_t   *key,
                                                  _Inout_ sai_attribute_value_t *value,
                                                  _In_ uint32_t                  attr_index,
                                                  _Inout_ vendor_cache_t        *cache,
                                                  void                          *arg)
{
    sx_status_t               sx_status;
    sai_status_t              status;
    rm_sdk_table_type_e       sx_rm_table_type;
    sai_object_type_t         object_type;
    sai_switch_attr_t         resource_type;
    sai_acl_resource_t        resource_info[SAI_ACL_BIND_POINT_TYPE_COUNT * 2];
    sai_acl_bind_point_type_t bind_point_type;
    sai_acl_stage_t           stage;
    uint32_t                  count, free_db_entries, free_acls;

    resource_type = (long)arg;

    assert((resource_type == SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE) ||
           (resource_type == SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP));

    memset(resource_info, 0, sizeof(resource_info));

    if (resource_type == SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE) {
        object_type      = SAI_OBJECT_TYPE_ACL_TABLE;
        sx_rm_table_type = RM_SDK_TABLE_TYPE_ACL_E;
    } else { /* SAI_SWITCH_ATTR_AVAILABLE_ACL_TABLE_GROUP */
        object_type      = SAI_OBJECT_TYPE_ACL_TABLE_GROUP;
        sx_rm_table_type = RM_SDK_TABLE_TYPE_ACL_GROUPS_E;
    }

    sx_status = sx_api_rm_free_entries_by_type_get(gh_sdk, sx_rm_table_type, &free_acls);
    if (SX_ERR(sx_status)) {
        SX_LOG_ERR("Failed to get a number of free resources for sx table %d - %s\n",
                   sx_rm_table_type, SX_STATUS_MSG(sx_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sx_status);
    }

    acl_global_lock();

    count = 0;
    for (bind_point_type = SAI_ACL_BIND_POINT_TYPE_PORT;
         bind_point_type < SAI_ACL_BIND_POINT_TYPE_COUNT;
         bind_point_type++) {
        for (stage = SAI_ACL_STAGE_INGRESS; stage <= SAI_ACL_STAGE_EGRESS; stage++) {
            resource_info[count].bind_point = bind_point_type;
            resource_info[count].stage      = stage;

            status = mlnx_acl_db_free_entries_get(object_type, &free_db_entries);
            if (SAI_ERR(status)) {
                goto out;
            }

            if ((bind_point_type == SAI_ACL_BIND_POINT_TYPE_VLAN) && (stage == SAI_ACL_STAGE_EGRESS)) {
                resource_info[count].avail_num = 0;
            } else {
                resource_info[count].avail_num = MIN(free_acls, free_db_entries);
            }

            count++;
        }
    }

    assert(count == ARRAY_SIZE(resource_info));

    status = mlnx_fill_aclresourcelist(resource_info, count, &value->aclresource);
    if (SAI_ERR(status)) {
        goto out;
    }

out:
    acl_global_unlock();
    SX_LOG_EXIT();
    return status;
}

/* Get the Max number of mirror session NPU supports [uint32_t] */
static sai_status_t mlnx_switch_max_mirror_sessions_get(_In_ const sai_object_key_t   *key,
                                                        _Inout_ sai_attribute_value_t *value,
                                                        _In_ uint32_t                  attr_index,
                                                        _Inout_ vendor_cache_t        *cache,
                                                        void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = g_resource_limits.span_session_id_max_external + 1;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Get the Max number of sampled mirror session NPU supports [uint32_t] */
static sai_status_t mlnx_switch_max_sampled_mirror_sessions_get(_In_ const sai_object_key_t   *key,
                                                                _Inout_ sai_attribute_value_t *value,
                                                                _In_ uint32_t                  attr_index,
                                                                _Inout_ vendor_cache_t        *cache,
                                                                void                          *arg)
{
    SX_LOG_ENTER();

    value->u32 = 0;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/* Get the list of supported get statistics extended modes [sai_s32_list_t] */
static sai_status_t mlnx_switch_supported_stats_modes_get(_In_ const sai_object_key_t   *key,
                                                          _Inout_ sai_attribute_value_t *value,
                                                          _In_ uint32_t                  attr_index,
                                                          _Inout_ vendor_cache_t        *cache,
                                                          void                          *arg)
{
    int32_t      modes[] = { SAI_STATS_MODE_READ, SAI_STATS_MODE_READ_AND_CLEAR };
    sai_status_t status;

    SX_LOG_ENTER();

    status = mlnx_fill_s32list(modes, sizeof(modes) / sizeof(*modes), &value->s32list);

    SX_LOG_EXIT();
    return status;
}

static sai_status_t mlnx_switch_crc_params_apply(bool init)
{
    sai_status_t        status;
    mlnx_port_config_t *port;
    uint32_t            ii;

    if (init) {
        /* No need to apply default values on init */
        if (g_sai_db_ptr->crc_check_enable && g_sai_db_ptr->crc_recalc_enable) {
            return SAI_STATUS_SUCCESS;
        }
    }

    mlnx_port_phy_foreach(port, ii) {
        status = mlnx_port_crc_params_apply(port, false);
        if (SAI_ERR(status)) {
            SX_LOG_ERR("Failed to update crc params for all ports\n");
            return status;
        }
    }

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_crc_check_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    SX_LOG_ENTER();
    sai_db_read_lock();

    value->booldata = g_sai_db_ptr->crc_check_enable;

    sai_db_unlock();
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_crc_check_set(_In_ const sai_object_key_t      *key,
                                              _In_ const sai_attribute_value_t *value,
                                              void                             *arg)
{
    sai_status_t status;
    bool         crc_check_enable_old;

    SX_LOG_ENTER();

    sai_db_write_lock();

    crc_check_enable_old = g_sai_db_ptr->crc_check_enable;

    g_sai_db_ptr->crc_check_enable = value->booldata;

    status = mlnx_switch_crc_params_apply(false);
    if (SAI_ERR(status)) {
        g_sai_db_ptr->crc_check_enable = crc_check_enable_old;
    }

    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

static sai_status_t mlnx_switch_crc_recalculation_get(_In_ const sai_object_key_t   *key,
                                                      _Inout_ sai_attribute_value_t *value,
                                                      _In_ uint32_t                  attr_index,
                                                      _Inout_ vendor_cache_t        *cache,
                                                      void                          *arg)
{
    SX_LOG_ENTER();
    sai_db_read_lock();

    value->booldata = g_sai_db_ptr->crc_recalc_enable;

    sai_db_unlock();
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_crc_recalculation_set(_In_ const sai_object_key_t      *key,
                                                      _In_ const sai_attribute_value_t *value,
                                                      void                             *arg)
{
    sai_status_t status;
    bool         crc_recalc_enable_old;

    SX_LOG_ENTER();

    sai_db_write_lock();

    crc_recalc_enable_old = g_sai_db_ptr->crc_recalc_enable;

    g_sai_db_ptr->crc_recalc_enable = value->booldata;

    status = mlnx_switch_crc_params_apply(false);
    if (SAI_ERR(status)) {
        g_sai_db_ptr->crc_recalc_enable = crc_recalc_enable_old;
    }

    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

static sai_status_t mlnx_switch_attr_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg)
{
    assert((long)arg == SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL);

    SX_LOG_ENTER();

    value->booldata = g_uninit_data_plane_on_removal;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_attr_set(_In_ const sai_object_key_t      *key,
                                         _In_ const sai_attribute_value_t *value,
                                         void                             *arg)
{
    assert((long)arg == SAI_SWITCH_ATTR_UNINIT_DATA_PLANE_ON_REMOVAL);

    SX_LOG_ENTER();

    g_uninit_data_plane_on_removal = value->booldata;

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_pre_shutdown_set(_In_ const sai_object_key_t      *key,
                                                 _In_ const sai_attribute_value_t *value,
                                                 void                             *arg)
{
    sx_status_t sx_status = SX_STATUS_SUCCESS;

    SX_LOG_ENTER();

    if (value->booldata) {
        sx_status = sx_api_issu_start_set(gh_sdk);
        if (SX_ERR(sx_status)) {
            SX_LOG_ERR("Failed to start issu %s.\n", SX_STATUS_MSG(sx_status));
        }
    }

    SX_LOG_EXIT();
    return sdk_to_sai(sx_status);
}

static sai_status_t mlnx_switch_init_connect_get(_In_ const sai_object_key_t   *key,
                                                 _Inout_ sai_attribute_value_t *value,
                                                 _In_ uint32_t                  attr_index,
                                                 _Inout_ vendor_cache_t        *cache,
                                                 void                          *arg)
{
    mlnx_object_id_t mlnx_switch_id = {0};
    sai_status_t     status;

    SX_LOG_ENTER();

    status = sai_to_mlnx_object_id(SAI_OBJECT_TYPE_SWITCH, key->key.object_id, &mlnx_switch_id);
    if (SAI_ERR(status)) {
        SX_LOG_EXIT();
        return status;
    }

    value->booldata = mlnx_switch_id.id.is_created;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_profile_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    SX_LOG_ENTER();
    value->u32 = g_profile_id;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_event_func_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    long attr_id = (long)arg;

    SX_LOG_ENTER();

    switch (attr_id) {
    case SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY:
        value->ptr = g_notification_callbacks.on_switch_state_change;
        break;

    case SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY:
        value->ptr = g_notification_callbacks.on_switch_shutdown_request;
        break;

    case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
        value->ptr = g_notification_callbacks.on_fdb_event;
        break;

    case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
        value->ptr = g_notification_callbacks.on_port_state_change;
        break;

    case SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY:
        value->ptr = g_notification_callbacks.on_packet_event;
        break;

    case SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY:
        value->ptr = g_notification_callbacks.on_bfd_session_state_change;
        break;
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

sai_status_t mlnx_switch_log_set(sx_verbosity_level_t level)
{
    sx_status_t status;

    LOG_VAR_NAME(__MODULE__) = level;

    if (gh_sdk) {
        if (!mlnx_chip_is_spc2or3()) {
            status = sx_api_issu_log_verbosity_level_set(gh_sdk, SX_LOG_VERBOSITY_BOTH, level, level);
            if (SX_ERR(status)) {
                MLNX_SAI_LOG_ERR("Set issu log verbosity failed - %s.\n", SX_STATUS_MSG(status));
                return sdk_to_sai(status);
            }
        }

        return sdk_to_sai(sx_api_topo_log_verbosity_level_set(gh_sdk, SX_LOG_VERBOSITY_BOTH, level, level));
    } else {
        return SAI_STATUS_SUCCESS;
    }
}

static sai_status_t mlnx_switch_total_pool_buffer_size_get(_In_ const sai_object_key_t   *key,
                                                           _Inout_ sai_attribute_value_t *value,
                                                           _In_ uint32_t                  attr_index,
                                                           _Inout_ vendor_cache_t        *cache,
                                                           void                          *arg)
{
    SX_LOG_ENTER();
    if (!value) {
        SX_LOG_ERR("NULL value\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    value->u64 = g_resource_limits.total_buffer_space / 1024;
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_ingress_pool_num_get(_In_ const sai_object_key_t   *key,
                                                     _Inout_ sai_attribute_value_t *value,
                                                     _In_ uint32_t                  attr_index,
                                                     _Inout_ vendor_cache_t        *cache,
                                                     void                          *arg)
{
    SX_LOG_ENTER();
    if (!value) {
        SX_LOG_ERR("NULL value\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    value->u32 = mlnx_sai_get_buffer_resource_limits()->num_ingress_pools;
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_egress_pool_num_get(_In_ const sai_object_key_t   *key,
                                                    _Inout_ sai_attribute_value_t *value,
                                                    _In_ uint32_t                  attr_index,
                                                    _Inout_ vendor_cache_t        *cache,
                                                    void                          *arg)
{
    SX_LOG_ENTER();
    if (!value) {
        SX_LOG_ERR("NULL value\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }
    value->u32 = mlnx_sai_get_buffer_resource_limits()->num_egress_pools;
    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_default_vlan_id_get(_In_ const sai_object_key_t   *key,
                                             _Inout_ sai_attribute_value_t *value,
                                             _In_ uint32_t                  attr_index,
                                             _Inout_ vendor_cache_t        *cache,
                                             void                          *arg)
{
    mlnx_object_id_t mlnx_vlan_id = {0};
    sai_status_t     status;

    SX_LOG_ENTER();

    mlnx_vlan_id.id.vlan_id = DEFAULT_VLAN;

    status = mlnx_object_id_to_sai(SAI_OBJECT_TYPE_VLAN, &mlnx_vlan_id, &value->oid);

    SX_LOG_EXIT();
    return status;
}

static sai_status_t mlnx_default_stp_id_get(_In_ const sai_object_key_t   *key,
                                            _Inout_ sai_attribute_value_t *value,
                                            _In_ uint32_t                  attr_index,
                                            _Inout_ vendor_cache_t        *cache,
                                            void                          *arg)
{
    sai_status_t status;

    SX_LOG_ENTER();

    /* return default STP */
    assert(NULL != g_sai_db_ptr);
    sai_db_read_lock();

    status = mlnx_create_object(SAI_OBJECT_TYPE_STP, mlnx_stp_get_default_stp(),
                                NULL, &value->oid);
    if (SAI_ERR(status)) {
        SX_LOG_ERR("Failed to create object of default STP id\n");
        goto out;
    }

out:
    sai_db_unlock();
    SX_LOG_EXIT();
    return status;
}

static sai_status_t mlnx_max_stp_instance_get(_In_ const sai_object_key_t   *key,
                                              _Inout_ sai_attribute_value_t *value,
                                              _In_ uint32_t                  attr_index,
                                              _Inout_ vendor_cache_t        *cache,
                                              void                          *arg)
{
    SX_LOG_ENTER();
    value->u32 = SX_MSTP_INST_ID_MAX - SX_MSTP_INST_ID_MIN + 1;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_qos_max_tcs_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg)
{
    SX_LOG_ENTER();
    value->u8 = g_resource_limits.cos_port_ets_traffic_class_max + 1;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_restart_type_get(_In_ const sai_object_key_t   *key,
                                          _Inout_ sai_attribute_value_t *value,
                                          _In_ uint32_t                  attr_index,
                                          _Inout_ vendor_cache_t        *cache,
                                          void                          *arg)
{
    SX_LOG_ENTER();
    value->s32 = SAI_SWITCH_RESTART_TYPE_ANY;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_min_restart_interval_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg)
{
    SX_LOG_ENTER();
    value->u32 = 0;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_nv_storage_get(_In_ const sai_object_key_t   *key,
                                         _Inout_ sai_attribute_value_t *value,
                                         _In_ uint32_t                  attr_index,
                                         _Inout_ vendor_cache_t        *cache,
                                         void                          *arg)
{
    SX_LOG_ENTER();
    /* SDK persistent files for ISSU approximate size */
    value->u64 = 100;
    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_event_func_set(_In_ const sai_object_key_t      *key,
                                               _In_ const sai_attribute_value_t *value,
                                               void                             *arg)
{
    long attr_id = (long)arg;

    SX_LOG_ENTER();

    switch (attr_id) {
    case SAI_SWITCH_ATTR_SWITCH_STATE_CHANGE_NOTIFY:
        g_notification_callbacks.on_switch_state_change = (sai_switch_state_change_notification_fn)value->ptr;
        break;

    case SAI_SWITCH_ATTR_SHUTDOWN_REQUEST_NOTIFY:
        g_notification_callbacks.on_switch_shutdown_request = (sai_switch_shutdown_request_notification_fn)value->ptr;
        break;

    case SAI_SWITCH_ATTR_FDB_EVENT_NOTIFY:
        g_notification_callbacks.on_fdb_event = (sai_fdb_event_notification_fn)value->ptr;
        break;

    case SAI_SWITCH_ATTR_PORT_STATE_CHANGE_NOTIFY:
        g_notification_callbacks.on_port_state_change = (sai_port_state_change_notification_fn)value->ptr;
        break;

    case SAI_SWITCH_ATTR_PACKET_EVENT_NOTIFY:
        g_notification_callbacks.on_packet_event = (sai_packet_event_notification_fn)value->ptr;
        break;

    case SAI_SWITCH_ATTR_BFD_SESSION_STATE_CHANGE_NOTIFY:
        g_notification_callbacks.on_bfd_session_state_change = (sai_bfd_session_state_change_notification_fn)value->ptr;
        break;
    }

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_transaction_mode_set(_In_ const sai_object_key_t      *key,
                                                     _In_ const sai_attribute_value_t *value,
                                                     void                             *arg)
{
    sx_status_t         sdk_status           = SX_STATUS_ERROR;
    sx_access_cmd_t     transaction_mode_cmd = SX_ACCESS_CMD_NONE;
    mlnx_port_config_t *port;
    uint32_t            ii;
    sx_status_t         sx_status;
    sx_vlan_ports_t     port_list;

    SX_LOG_ENTER();

    if (value->booldata) {
        transaction_mode_cmd = SX_ACCESS_CMD_ENABLE;
    } else {
        transaction_mode_cmd = SX_ACCESS_CMD_DISABLE;
    }

    sai_db_write_lock();

    if ((BOOT_TYPE_WARM == g_sai_db_ptr->boot_type) && !value->booldata && !g_sai_db_ptr->issu_end_called) {
        for (ii = 0; ii < MAX_PORTS * 2; ii++) {
            if (mlnx_ports_db[ii].is_present != mlnx_ports_db[ii].sdk_port_added) {
                SX_LOG_ERR("Presence of port of idx %d is not consistent in SAI (%d) and SDK (%d)\n",
                           ii,
                           mlnx_ports_db[ii].is_present,
                           mlnx_ports_db[ii].sdk_port_added);
                sai_db_unlock();
                SX_LOG_EXIT();
                return SAI_STATUS_FAILURE;
            }
            if (mlnx_ports_db[ii].sdk_port_added && (0 == mlnx_ports_db[ii].logical)) {
                sai_db_unlock();
                SX_LOG_ERR("LAG of idx %d is created in SDK but SAI db is not updated\n", ii);
                SX_LOG_EXIT();
                return SAI_STATUS_FAILURE;
            }
        }
        for (ii = 0; ii < MAX_PORTS; ii++) {
            if (mlnx_ports_db[ii].before_issu_lag_id != mlnx_ports_db[ii].lag_id) {
                SX_LOG_ERR("LAG member of idx %d does not belong to the same LAG in SAI (%x) SDK(%x)\n",
                           ii,
                           mlnx_ports_db[ii].lag_id,
                           mlnx_ports_db[ii].before_issu_lag_id);
                sai_db_unlock();
                SX_LOG_EXIT();
                return SAI_STATUS_FAILURE;
            }
        }

        sdk_status = sx_api_issu_end_set(gh_sdk);
        if (SX_STATUS_SUCCESS != sdk_status) {
            sai_db_unlock();
            SX_LOG_ERR("Failed to end issu\n");
            SX_LOG_EXIT();
            return sdk_to_sai(sdk_status);
        }
        g_sai_db_ptr->issu_end_called = true;

        mlnx_port_phy_foreach(port, ii) {
            /* Since router port belongs to vlan 1 in SDK, don't remove them from vlan membership on issu end.
             * When port is a LAG member, it is already cleaned when entering LAG. */
            if ((port->issu_remove_default_vid) && (!port->rifs) && (!mlnx_port_is_lag_member(port))) {
                memset(&port_list, 0, sizeof(port_list));
                port_list.log_port = port->logical;
                sx_status          = sx_api_vlan_ports_set(gh_sdk,
                                                           SX_ACCESS_CMD_DELETE,
                                                           DEFAULT_ETH_SWID,
                                                           DEFAULT_VLAN,
                                                           &port_list,
                                                           1);
                if (SX_ERR(sx_status)) {
                    sai_db_unlock();
                    SX_LOG_ERR("Failed to del default vlan ports %s.\n", SX_STATUS_MSG(sx_status));
                    SX_LOG_EXIT();
                    return sdk_to_sai(sdk_status);
                }
            }
        }

        mlnx_port_foreach(port, ii) {
            port->before_issu_lag_id = 0;
            port->sdk_port_added     = false;
        }
    }

    if (SX_STATUS_SUCCESS !=
        (sdk_status = sx_api_transaction_mode_set(gh_sdk, transaction_mode_cmd))) {
        sai_db_unlock();
        SX_LOG_ERR("Failed to set transaction mode to %d: %s\n", transaction_mode_cmd, SX_STATUS_MSG(sdk_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sdk_status);
    }

    g_sai_db_ptr->transaction_mode_enable = value->booldata;

    sai_db_unlock();

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_transaction_mode_get(_In_ const sai_object_key_t   *key,
                                                     _Inout_ sai_attribute_value_t *value,
                                                     _In_ uint32_t                  attr_index,
                                                     _Inout_ vendor_cache_t        *cache,
                                                     void                          *arg)
{
    SX_LOG_ENTER();

    sai_db_read_lock();

    value->booldata = g_sai_db_ptr->transaction_mode_enable;

    sai_db_unlock();

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_default_bridge_id_get(_In_ const sai_object_key_t   *key,
                                               _Inout_ sai_attribute_value_t *value,
                                               _In_ uint32_t                  attr_index,
                                               _Inout_ vendor_cache_t        *cache,
                                               void                          *arg)
{
    SX_LOG_ENTER();

    sai_db_read_lock();

    value->oid = mlnx_bridge_default_1q_oid();

    sai_db_unlock();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_vxlan_udp_port_set(uint16_t udp_port)
{
    sx_status_t                 sdk_status;
    sx_flex_parser_header_t     from;
    sx_flex_parser_transition_t to;

    SX_LOG_ENTER();

    memset(&from, 0, sizeof(from));
    memset(&to, 0, sizeof(to));

    from.parser_hdr_type           = SX_FLEX_PARSER_HEADER_FIXED_E;
    from.hdr_data.parser_hdr_fixed = SX_FLEX_PARSER_HEADER_UDP_E;

    to.next_parser_hdr.parser_hdr_type           = SX_FLEX_PARSER_HEADER_FIXED_E;
    to.next_parser_hdr.hdr_data.parser_hdr_fixed = SX_FLEX_PARSER_HEADER_VXLAN_E;
    to.encap_level                               = SX_FLEX_PARSER_ENCAP_OUTER_E;
    to.transition_value                          = udp_port;

    sdk_status = sx_api_flex_parser_transition_set(gh_sdk, SX_ACCESS_CMD_SET, from, to);
    if (SX_STATUS_SUCCESS != sdk_status) {
        SX_LOG_ERR("Failed to set vxlan default port: %s\n", SX_STATUS_MSG(sdk_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sdk_status);
    }

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_vxlan_default_port_set(_In_ const sai_object_key_t      *key,
                                                       _In_ const sai_attribute_value_t *value,
                                                       void                             *arg)
{
    sai_status_t sai_status = SAI_STATUS_FAILURE;

    SX_LOG_ENTER();

    sai_status = mlnx_switch_vxlan_udp_port_set(value->u16);
    if (SAI_ERR(sai_status)) {
        SX_LOG_ERR("Failed to set vxlan udp port value: %d\n", value->u16);
        SX_LOG_EXIT();
        return sai_status;
    }

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_vxlan_default_port_get(_In_ const sai_object_key_t   *key,
                                                       _Inout_ sai_attribute_value_t *value,
                                                       _In_ uint32_t                  attr_index,
                                                       _Inout_ vendor_cache_t        *cache,
                                                       void                          *arg)
{
    sx_status_t                 sdk_status = SX_STATUS_ERROR;
    sx_flex_parser_header_t     curr_ph;
    sx_flex_parser_transition_t next_trans_p;
    uint32_t                    next_trans_cnt;
    const uint32_t              udp_next_trans_cnt = 1;

    SX_LOG_ENTER();

    curr_ph.parser_hdr_type           = SX_FLEX_PARSER_HEADER_FIXED_E;
    curr_ph.hdr_data.parser_hdr_fixed = SX_FLEX_PARSER_HEADER_UDP_E;

    sdk_status = sx_api_flex_parser_transition_get(gh_sdk, curr_ph, NULL, &next_trans_cnt);
    if (SX_STATUS_SUCCESS != sdk_status) {
        SX_LOG_ERR("Failed to get vxlan default port: %s\n", SX_STATUS_MSG(sdk_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sdk_status);
    }

    if (udp_next_trans_cnt != next_trans_cnt) {
        SX_LOG_ERR("udp next trans cnt is %d but should be %d\n",
                   next_trans_cnt, udp_next_trans_cnt);
        SX_LOG_EXIT();
        return sdk_to_sai(sdk_status);
    }

    sdk_status = sx_api_flex_parser_transition_get(gh_sdk, curr_ph, &next_trans_p, &next_trans_cnt);
    if (SX_STATUS_SUCCESS != sdk_status) {
        SX_LOG_ERR("Failed to get vxlan default port: %s\n", SX_STATUS_MSG(sdk_status));
        SX_LOG_EXIT();
        return sdk_to_sai(sdk_status);
    }

    value->u16 = next_trans_p.transition_value;

    SX_LOG_EXIT();

    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_vxlan_default_router_mac_get(_In_ const sai_object_key_t   *key,
                                                             _Inout_ sai_attribute_value_t *value,
                                                             _In_ uint32_t                  attr_index,
                                                             _Inout_ vendor_cache_t        *cache,
                                                             void                          *arg)
{
    SX_LOG_ENTER();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

static sai_status_t mlnx_switch_vxlan_default_router_mac_set(_In_ const sai_object_key_t      *key,
                                                             _In_ const sai_attribute_value_t *value,
                                                             void                             *arg)
{
    SX_LOG_ENTER();

    SX_LOG_EXIT();
    return SAI_STATUS_SUCCESS;
}

/**
 * @brief Remove/disconnect Switch
 *   Release all resources associated with currently opened switch
 *
 * @param[in] switch_id The Switch id
 *
 * @return #SAI_STATUS_SUCCESS on success Failure status code on error
 */
static sai_status_t mlnx_remove_switch(_In_ sai_object_id_t switch_id)
{
    mlnx_object_id_t mlnx_switch_id = {0};
    sai_status_t     status;

    status = sai_to_mlnx_object_id(SAI_OBJECT_TYPE_SWITCH, switch_id, &mlnx_switch_id);
    if (SAI_ERR(status)) {
        return status;
    }

    if (g_uninit_data_plane_on_removal) {
        status = mlnx_shutdown_switch();
    } else {
        status = mlnx_disconnect_switch();
    }

    return status;
}

static uint32_t switch_stats_id_to_str(_In_ uint32_t             number_of_counters,
                                       _In_ const sai_stat_id_t *counter_ids,
                                       _In_ uint32_t             max_len,
                                       _Out_ char               *key_str)
{
    const char *base;
    uint32_t    idx, ii, pos = 0;

    assert(key_str);

    pos = snprintf(key_str, max_len, "[");
    if (pos > max_len) {
        return pos;
    }

    for (ii = 0; ii < number_of_counters; ii++) {
        if (counter_ids[ii] < SAI_SWITCH_STAT_IN_DROP_REASON_RANGE_END) {
            base = "IN_DROP_REASON_RANGE_BASE";
            idx  = counter_ids[ii] - SAI_SWITCH_STAT_IN_DROP_REASON_RANGE_BASE;
        } else {
            base = "OUT_DROP_REASON_RANGE_BASE";
            idx  = counter_ids[ii] - SAI_SWITCH_STAT_OUT_DROP_REASON_RANGE_BASE;
        }

        pos += snprintf(key_str + pos, max_len - pos, "%s_%d", base, idx);
        if (pos > max_len) {
            return pos;
        }

        if (ii < number_of_counters - 1) {
            pos += snprintf(key_str + pos, max_len - pos, ", ");
            if (pos > max_len) {
                return pos;
            }
        }
    }

    return snprintf(key_str + pos, max_len - pos, "]");
}

static void switch_stats_to_str(_In_ uint32_t             number_of_counters,
                                _In_ const sai_stat_id_t *counter_ids,
                                _In_ sai_stats_mode_t     mode,
                                _In_ uint32_t             max_len,
                                _Out_ char               *key_str)
{
    const char *mode_str;
    uint32_t    pos = 0;

    mode_str = sai_metadata_get_stats_mode_name(mode);

    pos = snprintf(key_str, max_len, "%s, ", mode_str);

    switch_stats_id_to_str(number_of_counters, counter_ids, max_len - pos, key_str + pos);
}

/**
 * @brief Get switch statistics counters extended.
 *
 * @param[in] switch_id Switch id
 * @param[in] number_of_counters Number of counters in the array
 * @param[in] counter_ids Specifies the array of counter ids
 * @param[in] mode Statistics mode
 * @param[out] counters Array of resulting counter values.
 *
 * @return #SAI_STATUS_SUCCESS on success, failure status code on error
 */
static sai_status_t mlnx_get_switch_stats_ext(_In_ sai_object_id_t      switch_id,
                                              _In_ uint32_t             number_of_counters,
                                              _In_ const sai_stat_id_t *counter_ids,
                                              _In_ sai_stats_mode_t     mode,
                                              _Out_ uint64_t           *counters)
{
    sai_status_t status;
    char         key_str[MAX_KEY_STR_LEN] = {0};
    uint32_t     ii;
    bool         clear;

    SX_LOG_ENTER();

    if (number_of_counters == 0) {
        SX_LOG_ERR("number_of_counters is 0\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (counter_ids == NULL) {
        SX_LOG_ERR("counter_ids is NULL\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (mode > SAI_STATS_MODE_READ_AND_CLEAR) {
        SX_LOG_ERR("mode %d is invalid\n", mode);
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (counters == NULL) {
        SX_LOG_ERR("counters is NULL\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    switch_stats_to_str(number_of_counters, counter_ids, mode, MAX_KEY_STR_LEN, key_str);
    SX_LOG_DBG("Get switch stats %s\n", key_str);

    for (ii = 0; ii < number_of_counters; ii++) {
        if (!MLNX_SWITCH_STAT_ID_RANGE_CHECK(counter_ids[ii])) {
            SX_LOG_ERR("Invalid switch stat id %d\n", counter_ids[ii]);
            SX_LOG_EXIT();
            return SAI_STATUS_INVALID_ATTRIBUTE_0 + ii;
        }
    }

    clear = (mode == SAI_STATS_MODE_READ_AND_CLEAR);

    status = mlnx_debug_counter_switch_stats_get(number_of_counters, counter_ids, true, clear, counters);

    SX_LOG_EXIT();
    return status;
}

/**
 * @brief Get switch statistics counters. Deprecated for backward compatibility.
 *
 * @param[in] switch_id Switch id
 * @param[in] number_of_counters Number of counters in the array
 * @param[in] counter_ids Specifies the array of counter ids
 * @param[out] counters Array of resulting counter values.
 *
 * @return #SAI_STATUS_SUCCESS on success, failure status code on error
 */
static sai_status_t mlnx_get_switch_stats(_In_ sai_object_id_t      switch_id,
                                          _In_ uint32_t             number_of_counters,
                                          _In_ const sai_stat_id_t *counter_ids,
                                          _Out_ uint64_t           *counters)
{
    sai_status_t status;

    SX_LOG_ENTER();

    status = mlnx_get_switch_stats_ext(switch_id, number_of_counters, counter_ids, SAI_STATS_MODE_READ, counters);

    SX_LOG_EXIT();
    return status;
}

/**
 * @brief Clear switch statistics counters.
 *
 * @param[in] switch_id Switch id
 * @param[in] number_of_counters Number of counters in the array
 * @param[in] counter_ids Specifies the array of counter ids
 *
 * @return #SAI_STATUS_SUCCESS on success, failure status code on error
 */
static sai_status_t mlnx_clear_switch_stats(_In_ sai_object_id_t      switch_id,
                                            _In_ uint32_t             number_of_counters,
                                            _In_ const sai_stat_id_t *counter_ids)
{
    sai_status_t status;
    char         key_str[MAX_KEY_STR_LEN] = {0};
    uint32_t     ii;

    SX_LOG_ENTER();

    if (number_of_counters == 0) {
        SX_LOG_ERR("number_of_counters is 0\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (counter_ids == NULL) {
        SX_LOG_ERR("counter_ids is NULL\n");
        SX_LOG_EXIT();
        return SAI_STATUS_INVALID_PARAMETER;
    }

    for (ii = 0; ii < number_of_counters; ii++) {
        if (!MLNX_SWITCH_STAT_ID_RANGE_CHECK(counter_ids[ii])) {
            SX_LOG_ERR("Invalid switch stat id %d\n", counter_ids[ii]);
            SX_LOG_EXIT();
            return SAI_STATUS_INVALID_ATTRIBUTE_0 + ii;
        }
    }

    switch_stats_id_to_str(number_of_counters, counter_ids, MAX_KEY_STR_LEN, key_str);
    SX_LOG_NTC("Clear switch stats %s\n", key_str);

    status = mlnx_debug_counter_switch_stats_get(number_of_counters, counter_ids, false, true, NULL);

    SX_LOG_EXIT();
    return status;
}

const sai_switch_api_t mlnx_switch_api = {
    mlnx_create_switch,
    mlnx_remove_switch,
    mlnx_set_switch_attribute,
    mlnx_get_switch_attribute,
    mlnx_get_switch_stats,
    mlnx_get_switch_stats_ext,
    mlnx_clear_switch_stats,
    NULL,
    NULL
};
