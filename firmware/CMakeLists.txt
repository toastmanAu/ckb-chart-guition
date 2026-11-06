cmake_minimum_required(VERSION 3.16)
# On-device light client: the protocol components of ckb-light-esp, pinned in
# ../upstream/ckb-light-esp (see docs/UPSTREAM_AUDIT.md for the commit). Only
# the five protocol components: its Wi-Fi and display components are not used.
set(CKB_LC_COMPONENTS ${CMAKE_CURRENT_LIST_DIR}/../upstream/ckb-light-esp/components)
set(EXTRA_COMPONENT_DIRS
    ${CKB_LC_COMPONENTS}/ckb_core
    ${CKB_LC_COMPONENTS}/ckb_transport
    ${CKB_LC_COMPONENTS}/ckb_protocol
    ${CKB_LC_COMPONENTS}/ckb_filter
    ${CKB_LC_COMPONENTS}/ckb_session)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ckb_chart)
