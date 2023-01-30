#!/bin/bash
# mesa build sh
# Copyright © Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.

cur_file_path=$(cd $(dirname "${0}");pwd)
cd ${cur_file_path}

link_dirs=(
    mesa
    llvm/llvm
    libdrm
)

error()
{
    echo -e  "\033[1;31m${*}\033[0m"
}

info()
{
    echo -e "\033[1;36m${*}\033[0m"
}

android_mesa_source_dirs="
    open_source/libdrm \
    unpack_open_source/llvm/llvm \
    unpack_open_source/mesa"

mesa_so_list=(
    system/lib/libInsLLVM.so
    system/lib64/libInsLLVM.so
    vendor/lib/libInsDrm.so
    vendor/lib64/libInsDrm.so
    vendor/lib/libinstrgbm.so
    vendor/lib64/libinstrgbm.so
    vendor/lib/libinstrglapi.so
    vendor/lib64/libinstrglapi.so
    vendor/lib/egl/libInstrGLES_mesa.so
    vendor/lib64/egl/libInstrGLES_mesa.so
    vendor/lib/dri/galliuminstr_dri.so
    vendor/lib64/dri/galliuminstr_dri.so
    vendor/lib/dri/swrast_dri.so
    vendor/lib64/dri/swrast_dri.so
)

android9_link()
{
    cd ..
    repo_path=$(pwd)
    rm -rf ${AN_AOSPDIR}/unpack_open_source
    ln -vs ${repo_path} ${AN_AOSPDIR}
    rm -rf ${AN_AOSPDIR}/open_source
    ln -vs ${repo_path}/../open_source ${AN_AOSPDIR}
    cd -
}

android11_copy()
{
    cd ..
    repo_path=$(pwd)
    [ ! -d ${AN_AOSPDIR}/VMIEngine/unpack_open_source ] && mkdir -p ${AN_AOSPDIR}/VMIEngine/unpack_open_source
    for link_dir in ${link_dirs[*]}
    do
        if [ "${link_dir}" == "libdrm" ];then
            rsync -azr --delete --exclude=".git" --exclude="Android.mk" ${repo_path}/${link_dir} ${AN_AOSPDIR}/VMIEngine/unpack_open_source
        elif [ "${link_dir}" == "llvm/llvm" ];then
            [ ! -d ${AN_AOSPDIR}/VMIEngine/unpack_open_source/llvm ] && mkdir -p ${AN_AOSPDIR}/VMIEngine/unpack_open_source/llvm
            rsync -azr --delete --exclude=".git" --exclude="test" ${repo_path}/${link_dir} ${AN_AOSPDIR}/VMIEngine/unpack_open_source/llvm
        else
            rsync -azr --delete --exclude=".git" ${repo_path}/${link_dir} ${AN_AOSPDIR}/VMIEngine/unpack_open_source
        fi
        [ ${?} != 0 ] && error "Failed to rsync ${link_dir} to  ${AN_AOSPDIR}" && return -1
    done
    [ -d ${AN_AOSPDIR}/VMIEngine/open_source ] && mkdir -p ${AN_AOSPDIR}/VMIEngine/open_source
    rsync -azr --delete --exclude=".git" ${repo_path}/../open_source/libdrm ${AN_AOSPDIR}/VMIEngine/open_source
    [ ${?} != 0 ] && error "Failed to rsync open_source/libdrm to  ${AN_AOSPDIR}" && return -1
    cd -
}

setup_env()
{
    export TOP=${AN_AOSPDIR}
    export OUT_DIR=${AN_AOSPDIR}/out
    export ANDROID_BUILD_TOP=${AN_AOSPDIR}
    if [ -z "${ANDROID_VERSION}" ];then
        android9_link
    else
        android11_copy
    fi
}

package()
{
    no_copy_symbol=$1
    output_dir=${MODULE_OUTPUT_DIR}
    output_symbols_dir=${MODULE_SYMBOL_DIR}
    [ -z "${output_dir}" ] && output_dir=${cur_file_path}/output && rm -rf ${output_dir} && mkdir -p ${output_dir}
    [ -z "${output_symbols_dir}" ] && output_symbols_dir=${cur_file_path}/output/symbols && rm -rf ${output_symbols_dir} && mkdir -p ${output_symbols_dir}
    for so_name in ${mesa_so_list[@]}
    do
        source_path=${AN_AOSPDIR}/out/target/product/generic_arm64/${so_name}
        source_symbol_path=${AN_AOSPDIR}/out/target/product/generic_arm64/symbols/${so_name}
        target_path=${output_dir}/${so_name%/*}
        [ ! -d "${target_path}" ] && mkdir -p ${target_path}
        symbol_target_path=${output_symbols_dir}/${so_name%/*}
        [ ! -d "${symbol_target_path}" ] && mkdir -p ${symbol_target_path}
        cp -d ${source_path} ${target_path}
        [ ${?} != 0 ] && error "Failed to copy ${so_name} to ${target_path}"
        [ -L ${source_path} ] && continue
        [ ${no_copy_symbol} == 1 ] && continue
        cp -d ${source_symbol_path} ${symbol_target_path}
        [ ${?} != 0 ] && error "Failed to copy ${so_name} to ${symbol_target_path}"
    done
    if [ -z "${MODULE_OUTPUT_DIR}" ];then
        cd output
        tar zcvf Mesa.tar.gz system vendor
        cd -
    fi
    if [ -z "${MODULE_SYMBOL_DIR}" ];then
        cd output/symbols
        tar zcvf ../MesaSymbols.tar.gz *
        cd -
    fi
}

clean()
{
    rm -rf output
}

inc()
{
    clean
    setup_env
    cd ${AN_AOSPDIR}
    source build/envsetup.sh
    lunch aosp_arm64-eng
    if [ ! -z "${ANDROID_VERSION}" ];then
        cd VMIEngine
    fi
    mmm ${android_mesa_source_dirs} -j
    [ ${?} != 0 ] && error "Failed to incremental compile ${android_mesa_source_dirs}" && return -1
    cd -
    package 0
}

build()
{
    inc
}

inc_empyt()
{
    package 1
}

ACTION=$1; shift
case "$ACTION" in
    build) build "$@";;
    inc) inc_empyt "$@";;
    *) error "input command[$ACTION] not support.";;
esac
