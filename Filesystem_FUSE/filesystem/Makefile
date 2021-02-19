all: 
	gcc -Wall -g ddfs.c log.c -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -pthread \
	 -lfuse -lssl -lcrypto \
	 -o ddfs

clean:
	rm -rf ./ddfs
