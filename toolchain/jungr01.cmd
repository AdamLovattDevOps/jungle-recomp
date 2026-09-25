cl /c /ASw /GD /GEf /Ox COUNTS.C > build.log
link /NOD COUNTS.OBJ, JUNGR01.DLL,, SDLLCEW LIBW, JUNGR01.DEF >> build.log
