all : rawdraw.exe rawdraw_tests.exe

#for X11 consider:             xorg-dev
#for X11, you will need:       libx-dev
#for full screen you'll need:  libxinerama-dev libxext-dev
#for OGL You'll need:          mesa-common-dev libglu1-mesa-dev

#-DCNFGRASTERIZER
#  and
#-CNFGOGL
#  are incompatible.


MINGW32:=C:/MinGW/bin/

rawdraw.exe : rawdraw\rawdraw.c
	$(MINGW32)gcc -g -o $@ $^  -lgdi32

rawdraw_tests.exe : rawdraw_tests.c
	$(MINGW32)gcc -Os -g -Irawdraw -INuklear -o $@ $^ -lgdi32

clean : 
	rm -rf *.o *~ rawdraw.exe rawdraw_tests.exe