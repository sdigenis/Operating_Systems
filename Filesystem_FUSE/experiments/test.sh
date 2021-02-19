#!/bin/sh

# This script should be executed in example directory
# Assumes mountdir and storagedir are already present

if [ $# -lt 2 ]
  then
    printf "%s\n" "Not enough arguments."
    printf "%s\n" "usage: ./test.sh [-v] MOUNTDIR STORAGEDIR"
    exit 1
fi

verb=false
if [ "$1" = "-v" ]; then
    verb=true
    mntdir="$2"
    strgdir="$3"
else
    mntdir="$1"
    strgdir="$2"
fi

bytes=4096
f1="smallfile"
f2="bigfile"
f1copy="smallfile_copy"

tmp="$(mktemp -d)"
dump="$tmp/dump"

pause()
{
    printf "%s\n" "$*"
    read -r _
}

printmount()
{
    printf "%s\n" "Mount dir: "
    ls -la "$mntdir"
    printf "%s\n" ""
}

printstorage()
{
    printf "%s\n" "Storage dir: "
    ls -la "$strgdir"
    printf "%s\n" ""
}

printrefcnt()
{
    find "$strgdir" -maxdepth 1 -type f ! \( -name "$f1" -o -name "$f2" \
        -o -name "$f1copy" \) | while read -r file
    do
        ref="$(tail -c4 "$file" | od -t u4 -An)"
        printf "Ref counter in $(basename "$file") (block file): %d\n" "$ref"
    done
}

printfiles()
{
    find "$strgdir" -maxdepth 1 -type f \( -name "$f1" -o -name "$f2" \
        -o -name "$f1copy" \) | while read -r file
    do
        printf "%s\n" "Hashes in "$file":"
        i=0
        n=21
        size="$(wc -c < "$file")"
        while [ "$((i * n))" != "$size" ]
        do
            hashline="$(dd bs="1" skip=$((i * n)) if="$file" status=none | od -N 20 -t x1 -An -w | tr -d ' ')"
            printf "%s\n" "$hashline"
            i=$((i + 1))
        done
        printf "\n"
    done
}

printdiv()
{
    printmount
    printstorage
    if [ "$verb" = true ] 
    then
        printfiles
    fi
    printrefcnt
    printf "%s\n" "---------------------------------------------------------------------------"
}

printf "%s\n" "Starting test..."

# file creation and write demo
# first file
pause "Press [Enter] to write $f1"
head -c $bytes /dev/zero | tr '\0' '1' > "$mntdir/$f1" && printf "%s\n\n" "Wrote $bytes B to $f1"
printdiv

# second file
pause "Press [Enter] to write $f2 with the same first block as $f1" 
head -c $bytes /dev/zero | tr '\0' '1' > "$mntdir/$f2" && printf "%s\n\n" "Wrote $bytes B to $f2"
printdiv

pause "Press [Enter] to write a new block at the end of $f2"
head -c $bytes /dev/zero | tr '\0' '2' >> "$mntdir/$f2" && printf "%s\n\n" "Wrote $bytes B to $f2"
printdiv

pause "Press [Enter] to write one more block at the end of $f2"
head -c $bytes /dev/zero | tr '\0' '3' >> "$mntdir/$f2" && printf "%s\n\n" "Wrote $bytes B to $f2"
printdiv

# overwrite a block demo
pause "Press [Enter] to overwrite $f2's second block with its third block"
head -c $bytes /dev/zero | tr '\0' '3' > "$dump"
dd if="$dump" of="$mntdir/$f2" seek=1 bs="$bytes" conv=notrunc status=none && printf "%s\n\n" "Wrote $bytes B to $f2"
printdiv


# copy demo
pause "Press [Enter] to copy $f1 in mountdir"
cp "$mntdir/$f1" "$mntdir/$f1copy" && printf "%s\n\n" "Copied $f1 to $mntdir: ($f1copy)"
printdiv

# rm demo
pause "Press [Enter] to remove $f2 from mountdir"
rm "$mntdir/$f2" && printf "%s\n\n" "Removed $f2"
printdiv

# clean up
rm "$dump"
rmdir "$tmp"