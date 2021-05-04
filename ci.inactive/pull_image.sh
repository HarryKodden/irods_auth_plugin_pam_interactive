#!/bin/bash

docker pull $( echo $DOCKER_IMAGE | \
                   sed 's/__IRODS_VERSION__/'$( echo $IRODS_VERSION | tr '.' '_' )'/g' )

