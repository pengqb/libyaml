#!/bin/sh

#以下定制特殊的升级流程

#crond_publish=`cat /etc/crontab | grep 'publish_check.sh'`
#if [ "x" == "x$crond_publish" ];then
#        echo "*/1 * * * * root /waf/console/scripts/publish_check.sh" >> /etc/crontab
#fi

echo "install ok"
