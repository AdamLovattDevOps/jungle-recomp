cl /c /AS /Gw TEST.C > build.log
link /NOD TEST.OBJ, TEST.DLL,, SDLLCEW LIBW, TEST.DEF >> build.log
