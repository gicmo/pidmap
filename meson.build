project('pidmap', 'c',   license : 'LGPL-2.1+',
	default_options: ['warning_level=1',
			  'c_std=gnu11',
			  'buildtype=debugoptimized'])

glib    = dependency('glib-2.0', version: '>= 2.50.0')
gio     = dependency('gio-2.0')
unix    = dependency('gio-unix-2.0')

executable('pidmap', ['pidmap.c'], dependencies: [glib, gio, unix])
