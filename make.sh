#!/bin/sh

opt=()
if [ "$1" = "-o" ]; then
	opt=( "-O2" )
fi

#gcc "${opt[@]}" -std=c23 -ggdb3 -march=native -o nullcombine  nullcombine.c
gcc "${opt[@]}" -std=c23 -ggdb3 -march=native -o nulldiff  nulldiff.c
gcc "${opt[@]}" -std=c23 -ggdb3 -march=native -o hashole  hashole.c
gcc "${opt[@]}" -std=c23 -ggdb3 -march=native -o hasnull  hasnull.c

