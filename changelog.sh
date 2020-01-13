#!/bin/bash
# Author:Andrey Nikishaev
# Modified: Andrea Baldan
echo "CHANGELOG"
echo ----------------------
git for-each-ref --sort=-taggerdate --format '%(tag)-%(*authordate)' refs/tags \
    | grep -P "v[\d+\.]+" \
    | while read RES ;
do
    IFS='-' read -a arrRES <<< "${RES}"
    TAG=${arrRES[0]}
    DAT=${arrRES[1]}
    echo
    if [ $NEXT ];then
        echo [$NEXT] - $DAT
    else
        echo "[Current]" - $DAT
    fi
    GIT_PAGER=cat git log --no-merges --author="Andrea" --format=" * %s" $TAG..$NEXT
    NEXT=$TAG
done
echo
