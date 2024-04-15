#
#  Copyright (c) 2018 Lyude Paul <thatslyude@gmail.com>
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
#  SOFTWARE.
#

cc = meson.get_compiler('c')
dep_m = cc.find_library('m', required : false)
dep_egl = dependency('egl', required : false)
dep_gles2 = dependency('glesv2', required : false)
cube_c_args = []

#mesondefine HAS_RUNCUBE
if dep_m.found() and dep_egl.found() and dep_gles2.found()
	runcube_way = library('runcube_way',
		'runcube.c',
		'common.c',
		'cube-smooth.c',
		'esTransform.c',
		c_args : cube_c_args,
		include_directories : '..',
		dependencies : [
			dep_m,
			threads_dep,
			libdrm_dep,
			dep_egl,
			dep_gles2
		],
	)
	runcube_opt = [runcube_way]
	conf_data.set10('HAS_RUNCUBE', true)
else
	message('Runcube library not built - not all required libs found')
	runcube_opt = []
	conf_data.set10('HAS_RUNCUBE', false)
endif

