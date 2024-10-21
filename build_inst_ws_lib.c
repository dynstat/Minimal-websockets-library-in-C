cl /c ws_lib.c

lib ws_lib.obj

cl example_usage.c ws_lib.lib
example_usage.exe