# file      : buildfile
# copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

./: build2/ tests/ unit-tests/ doc/ \
  doc{INSTALL LICENSE NEWS README version} \
  file{bootstrap.sh bootstrap-msvc.bat bootstrap-mingw.bat} \
  file{INSTALL.cli config.guess config.sub manifest}

doc{version}: file{manifest} # Generated by the version module.
doc{version}: dist = true

# Don't install tests or the INSTALL file.
#
dir{tests/}:      install = false
dir{unit-tests/}: install = false
doc{INSTALL}@./:  install = false
