all:
	mkdir -p mountdir
	mkdir -p storagedir
	echo "bogus file" > storagedir/bogus.txt

umount:
	fusermount -u mountdir

clean: 
	rm -rf mountdir storagedir ddfs.log
