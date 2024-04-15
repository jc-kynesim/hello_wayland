freetype_dep = dependency('freetype2', required : false)

#mesondefine HAS_RUNTICKER
conf_data.set10('HAS_RUNTICKER', freetype_dep.found())
runticker_opt = []

if freetype_dep.found()
	runticker_way = library('runticker_way',
		'runticker.c', 'ticker.c',
		include_directories : '..',
		dependencies : [
			threads_dep,
			freetype_dep,
			libdrm_dep
		],
	)
	runticker_opt = [runticker_way]
else
	message('Not building ticker - freetype not found')
endif

