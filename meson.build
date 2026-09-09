project(
	'paup',
	'c',
	version: '5',
	meson_version: '>=1.0.0',
	default_options: [
		'buildtype=debugoptimized',
		'c_std=c11',
		'warning_level=3',
	],
)

# POSIX clock and sleep declarations are used by the key-repeat timer.
add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

configuration = configuration_data()
configuration.set_quoted('PAUP_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: configuration)

paup_dependencies = [
	dependency('libpulse'),
	dependency('xcb'),
]

executable(
	'paup',
	[
		'paup.c',
		'pulse.c',
	],
	dependencies: paup_dependencies,
	install: true,
)
