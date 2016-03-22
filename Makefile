DEBUG_LEVEL=3

ifneq ($(DEBUG_LEVEL),0)
export DEBUG_OPTS=-DDEBUG -DDEBUG_LEVEL=${DEBUG_LEVEL}
endif


all:
	cd LASlib && make
	cd LASzip && make
	cd src && make
#	cd src_full && make

clean:
	cd LASlib && make clean
	cd LASzip && make clean
	cd src && make clean
#	cd src_full && make clean
	cd bin && rm -rf *
    
clobber:
	cd LASlib && make clobber
	cd LASzip && make clobber
	cd src && make clobber
#	cd src_full && make clobber
	cd bin && rm -rf *
	