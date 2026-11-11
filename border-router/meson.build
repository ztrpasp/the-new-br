project('border-router', 'c',
  version : '0.1.0',
  default_options : ['buildtype=release', 'c_args=-w']
)

cc = meson.get_compiler('c')
dpdk_dep = dependency('libdpdk', version : '>=21.08.0', method : 'pkg-config')
sqlite3_dep = dependency('sqlite3', version : '>=3.38.0', method : 'pkg-config')
openssl_dep = dependency('openssl', version : '1.1.1m', method : 'pkg-config')
m_dep = cc.find_library('m', required : true)

add_project_arguments('-DALLOW_EXPERIMENTAL_API', language : 'c')

if get_option('perf_test')
  add_project_arguments('-DPERFORMANCE_TEST', language : 'c')
endif
if get_option('no_rate_recording')
  add_project_arguments('-DNO_RATE_RECORDING', language : 'c')
endif
if get_option('profiler_enable')
  add_project_arguments('-DPROFILER_ENABLE', language : 'c')
endif
if get_option('no_pid_calculation')
  add_project_arguments('-DNO_PID_CALCULATION', language : 'c')
endif
if get_option('no_get_cksum')
  add_project_arguments('-DNO_GET_CKSUM', language : 'c')
endif
if get_option('no_hmac_computation')
  add_project_arguments('-DNO_HMAC_COMPUTATION', language : 'c')
endif
if get_option('no_hmac_verification')
  add_project_arguments('-DNO_HMAC_VERIFICATION', language : 'c')
endif
if get_option('nidt_test')
  add_project_arguments('-DNIDT_TEST', language : 'c')
endif
if get_option('odct_test')
  add_project_arguments('-DODCT_TEST', language : 'c')
endif

pref_opt = get_option('prefetch')
if pref_opt == 'none'
  add_project_arguments('-DPREFETCH_NONE', language : 'c')
elif pref_opt == 'one'
  add_project_arguments('-DPREFETCH_ONE', language : 'c')
elif pref_opt == 'two'
  add_project_arguments('-DPREFETCH_TWO', language : 'c')
else
  add_project_arguments('-DPREFETCH_THREE', language : 'c')
endif

common_src = files(
  'load_conf.c',
  'br_tables.c',
  'br_conf.c',
  'utils/utils.c',
  'profiler/profiler.c',
  'arp/arp.c',
  'packet/rte_CoLoR.c',
  'init.c',
  'tables/arp_table.c',
  'tables/ip_lpm_table.c',
  'tables/nid_table.c',
  'tables/odc_table.c',
  'tables/peer_table.c',
  'tables/port_table.c',
  'tables/px_table.c',
  'tables/stats_table.c',
  'commandline/cli.c',
  'commandline/cmd_parser.c'
)
rtc_src = files('main.c', 'br-rtc.c')
pipeline0_src = files('br-pipeline0-main.c', 'br-pipeline0.c')

executable('br',
  common_src + rtc_src,
  dependencies : [dpdk_dep, sqlite3_dep, openssl_dep, m_dep],
  install : true
)

executable('br-static',
  common_src + rtc_src,
  dependencies : [dpdk_dep, sqlite3_dep, openssl_dep, m_dep],
  install : true
)

executable('br-pipeline0',
  common_src + pipeline0_src,
  dependencies : [dpdk_dep, sqlite3_dep, openssl_dep, m_dep],
  install : true
)

