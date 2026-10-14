#!/bin/sh
# List streams in an sghooks sound log (SGHOOKS_SNDLOG=1) that kept requesting
# refills every mixer tick for more than 5 s: streams stuck replaying their ring.
for log in "$@"; do
    echo "== $log"
    grep -a "snd post" "$log" | awk '{s=$5; if(!(s in f)) f[s]=$1; l[s]=$1; n[s]++}
        END{k=0; for(s in f){d=(l[s]-f[s])/1000; if(d>5 && n[s]/d>15){printf "  stuck %s for %.0f s (%d requests)\n", s, d, n[s]; k++}}
            if(!k) print "  no stuck streams"}'
done
