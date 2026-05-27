project('xfruncat', 'c',
    version: '2.1.0',
    license: 'MIT',
    default_options: ['warning_level=2']
)

cc = meson.get_compiler('c')

gtk3_dep = dependency('gtk+-3.0', version: '>= 3.22')
libxfce4panel_dep = dependency('libxfce4panel-2.0', version: '>= 4.12')
libxfce4util_dep = dependency('libxfce4util-1.0', version: '>= 4.12')
pangocairo_dep = dependency('pangocairo', version: '>= 1.42')

has_pango_font_map_add_font_file = cc.has_header_symbol(
    'pango/pango.h',
    'pango_font_map_add_font_file',
    dependencies: pangocairo_dep,
)

libxfconf_dep = dependency('libxfconf-0')
m_dep = cc.find_library('m', required: false)

plugin_deps = [
    gtk3_dep,
    libxfce4panel_dep,
    libxfce4util_dep,
    pangocairo_dep,
    libxfconf_dep,
    m_dep,
]

if not has_pango_font_map_add_font_file
    plugin_deps += dependency('fontconfig')
endif

font_src_dir = join_paths(meson.current_source_dir(), 'res')
font_install_dir = join_paths(get_option('prefix'), get_option('datadir'), 'xfce4', 'xfruncat')

shared_module('xfruncat',
    'src/xfruncat.c',
    dependencies: plugin_deps,
    c_args: [
        '-DG_LOG_DOMAIN="xfruncat"',
        '-DPROJECT_VERSION="@0@"'.format(meson.project_version()),
        '-DPOLYCAT_FONT_SRC_DIR="' + font_src_dir + '"',
        '-DPOLYCAT_FONT_INSTALL_DIR="' + font_install_dir + '"',
    ] + (has_pango_font_map_add_font_file ? ['-DHAVE_PANGO_FONT_MAP_ADD_FONT_FILE'] : []),
    install: true,
    install_dir: join_paths(get_option('libdir'), 'xfce4', 'panel', 'plugins')
)

install_data('xfruncat.desktop',
    install_dir: join_paths(get_option('datadir'), 'xfce4', 'panel', 'plugins')
)

install_data('res/polycat.ttf',
    install_dir: join_paths(get_option('datadir'), 'xfce4', 'xfruncat')
)
