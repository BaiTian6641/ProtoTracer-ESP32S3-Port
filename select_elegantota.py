# Select ElegantOTA dependency at pre-build time if local ElegantOTAPro is missing.
# This script runs early in the build (pre: stage) and appends the ElegantOTA
# git dependency only when the local ElegantOTAPro directory is not present.

from os.path import isdir, join
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()
project_dir = env['PROJECT_DIR']
local_pro = join(project_dir, 'lib', 'ElegantOTAPro-3-1-4')
if isdir(local_pro):
    print('Using local ElegantOTAPro (no extra ElegantOTA lib added)')
else:
    print('No local ElegantOTAPro found; adding ElegantOTA git library to LIBDEPS')
    env.Append(LIBDEPS=['https://github.com/ayushsharma82/ElegantOTA.git'])
