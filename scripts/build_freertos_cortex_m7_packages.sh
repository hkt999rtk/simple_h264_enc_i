#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/build_freertos_cortex_m7_packages.sh [--version X.Y.Z] [--output-dir DIR] [--build-dir DIR]

Build and validate FreeRTOS Cortex-M7 developer packages for the production
static library. The script emits both soft-float and fpv5-sp-d16 hard-float
package archives plus .sha256 checksum files.
EOF
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
version=""
output_dir="${repo_dir}/build/freertos-cortex-m7-packages"
build_dir=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)
            if [[ $# -lt 2 ]]; then
                echo "--version requires a value" >&2
                exit 2
            fi
            version="$2"
            shift 2
            ;;
        --output-dir)
            if [[ $# -lt 2 ]]; then
                echo "$1 requires a value" >&2
                exit 2
            fi
            output_dir="$2"
            shift 2
            ;;
        --out-dir)
            if [[ $# -lt 2 ]]; then
                echo "$1 requires a value" >&2
                exit 2
            fi
            output_dir="$2"
            shift 2
            ;;
        --build-dir)
            if [[ $# -lt 2 ]]; then
                echo "--build-dir requires a value" >&2
                exit 2
            fi
            build_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${version}" ]]; then
    version="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*simple_h264_enc_i[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*$/\1/p' "${repo_dir}/CMakeLists.txt")"
fi
if [[ -z "${version}" ]]; then
    echo "Could not determine project version from CMakeLists.txt" >&2
    exit 1
fi

cc="${ARM_NONE_EABI_GCC:-arm-none-eabi-gcc}"
ar="${ARM_NONE_EABI_AR:-arm-none-eabi-ar}"
nm="${ARM_NONE_EABI_NM:-arm-none-eabi-nm}"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Required tool not found: $1" >&2
        exit 1
    fi
}

require_tool "${cc}"
require_tool "${ar}"
require_tool "${nm}"
require_tool tar
require_tool gzip

if command -v sha256sum >/dev/null 2>&1; then
    checksum_file() {
        sha256sum "$1"
    }
elif command -v shasum >/dev/null 2>&1; then
    checksum_file() {
        shasum -a 256 "$1"
    }
else
    echo "Required SHA-256 tool not found: sha256sum or shasum" >&2
    exit 1
fi

mkdir -p "${output_dir}"
output_dir="$(cd "${output_dir}" && pwd)"
if [[ -z "${build_dir}" ]]; then
    build_dir="${output_dir}/_build"
fi
mkdir -p "${build_dir}"
build_root="$(cd "${build_dir}" && pwd)"
package_root="${output_dir}/_packages"
rm -rf "${build_root}" "${package_root}"
mkdir -p "${build_root}" "${package_root}"

git_commit="$(git -C "${repo_dir}" rev-parse HEAD 2>/dev/null || printf 'unknown')"
cc_path="$(command -v "${cc}")"
ar_path="$(command -v "${ar}")"
nm_path="$(command -v "${nm}")"
cc_version="$("${cc}" --version | sed -n '1,3p')"

common_flags=(
    -mcpu=cortex-m7
    -mthumb
    -O2
    -ffreestanding
    -fno-builtin
    -ffunction-sections
    -fdata-sections
    -DNJ_USE_LIBC=0
    -DSH264E_ENABLE_JPEG_TEST_HOOKS=0
    "-I${repo_dir}/include"
)

required_symbols=(
    sh264e_encode_jpeg_idr_with_arena_stream
    sh264e_encode_jpeg_source_idr_with_arena_stream
    sh264e_jpeg_source_get_work_size
    sh264e_jpeg_source_get_slice_work_size
    sh264e_jpeg_get_last_allocation_stats
    sh264e_jpeg_get_last_streaming_cache_bytes
    sh264e_jpeg_get_last_slice_work_bytes
    sh264e_encoder_get_memory_report
)

forbidden_symbol_regex='(^|[^A-Za-z0-9_])_?(sh264e_jpeg_set_test_allocation_limit|sh264e_jpeg_get_test_last_exact_resize_mask|sh264e_encode_jpeg_idr_streaming_prototype|sh264e_encode_jpeg_idr_streaming_prototype_with_arena|njDecodeComponents|njGetImage|njGetImageSize|njIsColor|njDecode)([^A-Za-z0-9_]|$)'

write_manifest() {
    local root="$1"
    (
        cd "${root}"
        {
            printf '# SHA-256 manifest\n'
            printf '# manifest.txt is excluded to avoid a self-referential checksum.\n'
            find . -type f ! -name manifest.txt -print | LC_ALL=C sort | while IFS= read -r file; do
                checksum_file "${file}"
            done
        } > manifest.txt
    )
}

validate_package() {
    local archive="$1"
    local package_name="$2"
    local lib_path="$3"
    local nm_out="$4"

    if [[ ! -s "${archive}" ]]; then
        echo "Package archive is missing or empty: ${archive}" >&2
        exit 1
    fi

    local listing="${archive}.listing"
    tar -tzf "${archive}" | tee "${listing}" >/dev/null

    local required_path
    for required_path in \
        "${package_name}/include/sh264e.h" \
        "${package_name}/lib/libsimple_h264_enc_i.a" \
        "${package_name}/docs/README-FREERTOS-CORTEX-M7.md" \
        "${package_name}/docs/API.md" \
        "${package_name}/docs/MEMORY.md" \
        "${package_name}/build-info.txt" \
        "${package_name}/manifest.txt"
    do
        if ! grep -Fxq "${required_path}" "${listing}"; then
            echo "Package ${archive} is missing required path: ${required_path}" >&2
            exit 1
        fi
        if [[ ! -s "${package_root}/${required_path}" ]]; then
            echo "Package ${archive} has an empty required path: ${required_path}" >&2
            exit 1
        fi
    done
    rm -f "${listing}"

    "${ar}" t "${lib_path}" | grep -Fxq sh264e.o
    "${ar}" t "${lib_path}" | grep -Fxq nanojpeg.o
    "${nm}" -g --defined-only "${lib_path}" > "${nm_out}"

    local symbol
    for symbol in "${required_symbols[@]}"; do
        if ! grep -Eq "(^|[^A-Za-z0-9_])_?${symbol}([^A-Za-z0-9_]|$)" "${nm_out}"; then
            echo "Production package is missing public symbol: ${symbol}" >&2
            exit 1
        fi
    done

    if grep -Eq "${forbidden_symbol_regex}" "${nm_out}"; then
        echo "Production package exposes a forbidden private/test symbol:" >&2
        grep -E "${forbidden_symbol_regex}" "${nm_out}" >&2
        exit 1
    fi
}

build_variant() {
    local abi="$1"
    local variant_flags_text="$2"
    shift 2
    local variant_flags=("$@")

    local build_dir="${build_root}/${abi}"
    local package_name="simple_h264_enc_i-${version}-freertos-cortex-m7-${abi}"
    local package_dir="${package_root}/${package_name}"
    local archive="${output_dir}/${package_name}.tar.gz"
    local lib_path="${package_dir}/lib/libsimple_h264_enc_i.a"

    mkdir -p "${build_dir}" "${package_dir}/include" "${package_dir}/lib" "${package_dir}/docs"

    "${cc}" "${common_flags[@]}" "${variant_flags[@]}" -std=c99 \
        -c "${repo_dir}/src/sh264e.c" \
        -o "${build_dir}/sh264e.o"
    "${cc}" "${common_flags[@]}" "${variant_flags[@]}" -std=c99 \
        -c "${repo_dir}/src/nanojpeg.c" \
        -o "${build_dir}/nanojpeg.o"
    "${ar}" rcs "${lib_path}" "${build_dir}/sh264e.o" "${build_dir}/nanojpeg.o"

    cp "${repo_dir}/include/sh264e.h" "${package_dir}/include/sh264e.h"
    cp "${repo_dir}/docs/freertos-cortex-m7/README-FREERTOS-CORTEX-M7.md" "${package_dir}/docs/README-FREERTOS-CORTEX-M7.md"
    cp "${repo_dir}/docs/freertos-cortex-m7/API.md" "${package_dir}/docs/API.md"
    cp "${repo_dir}/docs/freertos-cortex-m7/MEMORY.md" "${package_dir}/docs/MEMORY.md"

    cat > "${package_dir}/build-info.txt" <<EOF
package: ${package_name}
version: ${version}
git_commit: ${git_commit}
compiler: ${cc_path}
archiver: ${ar_path}
nm: ${nm_path}
compiler_version:
${cc_version}
target_cpu_flags: -mcpu=cortex-m7 -mthumb
target_fpu_flags: ${variant_flags_text}
compile_flags: ${common_flags[*]} ${variant_flags_text}
archive_variant: freertos-cortex-m7-${abi}
sources:
  - src/sh264e.c
  - src/nanojpeg.c
headers:
  - include/sh264e.h
production_policy:
  - tools disabled
  - tests disabled
  - private JPEG test hooks disabled
  - NJ_USE_LIBC=0
EOF

    write_manifest "${package_dir}"

    rm -f "${archive}" "${archive}.sha256"
    tar -C "${package_root}" -cf - "${package_name}" | gzip -n > "${archive}"
    checksum_file "${archive}" > "${archive}.sha256"

    validate_package "${archive}" "${package_name}" "${lib_path}" "${build_dir}/defined-symbols.txt"
    printf '%s\n' "${archive}"
    printf '%s\n' "${archive}.sha256"
}

build_variant "soft" "-mfloat-abi=soft" -mfloat-abi=soft
build_variant "fpv5-sp-hard" "-mfpu=fpv5-sp-d16 -mfloat-abi=hard" -mfpu=fpv5-sp-d16 -mfloat-abi=hard
