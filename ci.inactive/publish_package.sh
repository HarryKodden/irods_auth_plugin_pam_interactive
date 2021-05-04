#!/bin/bash
set -x
set -e
TARGET_DIR=/data/RPMS/$CI_PROJECT_NAMESPACE/${CI_COMMIT_REF_NAME}/CentOS/7/irods-${IRODS_VERSION}/x86_64
RPM=${PACKAGE_NAME}-${VERSION}-${CI_PIPELINE_ID}.x86_64.rpm
TRPM=${PACKAGE_NAME}-${VERSION}-${CI_PIPELINE_ID}.x86_64.rpm
REMOTE_TARGET_DIR=Centos/7/irods-${IRODS_VERSION}/${CI_COMMIT_REF_NAME}/x86_64/Packages
for IREPO in $( echo ${REPO} | tr ',' ' ' )
do
    curl -H "X-JFrog-Art-Api:$ARTIE_KEY" \
         -XPUT https://artie.ia.surfsara.nl/artifactory/${IREPO}/${REMOTE_TARGET_DIR}/${TRPM} -T $TARGET_DIR/${RPM}
done

