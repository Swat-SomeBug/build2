# file      : buildfile
# copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
# license   : MIT; see accompanying LICENSE file

d = build2/ tests/
./: $d doc{INSTALL LICENSE version} file{INSTALL.cli bootstrap manifest}
include $d

doc{INSTALL*}: install = false
