project('pidmap', 'c',
	version: '0.1',
	license : 'LGPL-2.1+',
	default_options: ['warning_level=1',
			  'c_std=gnu11',
			  'buildtype=debugoptimized'])

glib    = dependency('glib-2.0', version: '>= 2.50.0')
gio     = dependency('gio-2.0')
unix    = dependency('gio-unix-2.0')

conf = configuration_data()
conf.set('_GNU_SOURCE', true)
conf.set_quoted('PACKAGE_NAME', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
configure_file(output : 'config.h', configuration: conf)

executable('pidmap', ['pidmap.c'], dependencies: [glib, gio, unix, json])
