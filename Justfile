default:
    @just --list --unsorted

config := absolute_path('config')
build := absolute_path('.build')
out := absolute_path('firmware')
draw := absolute_path('draw')

# parse combos.dtsi and adjust settings to not run out of slots
# _parse_combos:
#     #!/usr/bin/env bash
#     set -euo pipefail
#     cconf="{{ config / 'combos.dtsi' }}"
#     if [[ -f $cconf ]]; then
#         # set MAX_COMBOS_PER_KEY to the most frequent combos count
#         count=$(
#             tail -n +10 $cconf |
#                 grep -Eo '[LR][TMBH][0-9]' |
#                 sort | uniq -c | sort -nr |
#                 awk 'NR==1{print $1}'
#         )
#         sed -Ei "/CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY/s/=.+/=$count/" "{{ config }}"/*.conf
#         echo "Setting MAX_COMBOS_PER_KEY to $count"
#
#         # set MAX_KEYS_PER_COMBO to the most frequent key count
#         count=$(
#             tail -n +10 $cconf |
#                 grep -o -n '[LR][TMBH][0-9]' |
#                 cut -d : -f 1 | uniq -c | sort -nr |
#                 awk 'NR==1{print $1}'
#         )
#         sed -Ei "/CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO/s/=.+/=$count/" "{{ config }}"/*.conf
#         echo "Setting MAX_KEYS_PER_COMBO to $count"
#     fi

# parse build.yaml and filter targets by expression
_parse_targets $expr:
    #!/usr/bin/env bash
    attrs="[.board, .shield, .snippet]"
    filter="(($attrs | map(. // [.]) | combinations), ((.include // {})[] | $attrs)) | join(\",\")"
    echo "$(yq -r "$filter" build.yaml | grep -v "^," | grep -i "${expr/#all/.*}")"

# build firmware for single board & shield combination
_build_single $board $shield $snippet *west_args:
    #!/usr/bin/env bash
    set -euo pipefail
    artifact="${shield:+${shield// /+}-}${board}"
    build_dir="{{ build / '$artifact' }}"

    echo "Building firmware for $artifact..."
    west build -s zmk/app -d "$build_dir" -b $board {{ west_args }} ${snippet:+-S "$snippet"} -- \
        -DZMK_CONFIG="{{ config }}" ${shield:+-DSHIELD="$shield"}

    if [[ -f "$build_dir/zephyr/zmk.uf2" ]]; then
        mkdir -p "{{ out }}" && cp "$build_dir/zephyr/zmk.uf2" "{{ out }}/$artifact.uf2"
    else
        mkdir -p "{{ out }}" && cp "$build_dir/zephyr/zmk.bin" "{{ out }}/$artifact.bin"
    fi

# build firmware for matching targets
# build expr *west_args: _parse_combos
build expr *west_args: 
    #!/usr/bin/env bash
    set -euo pipefail
    targets=$(just _parse_targets {{ expr }})

    [[ -z $targets ]] && echo "No matching targets found. Aborting..." >&2 && exit 1
    echo "$targets" | while IFS=, read -r board shield snippet; do
        just _build_single "$board" "$shield" "$snippet" {{ west_args }}
    done

# clear build cache and artifacts
clean:
    rm -rf {{ build }} {{ out }}

# clear all automatically generated files
clean-all: clean
    rm -rf .west zmk

# clear nix cache
clean-nix:
    nix-collect-garbage --delete-old

# parse & plot keymap
# usage:
#   just draw          # full draw + per-layer svgs
#   just draw single   # fast draw of combined svg only
draw mode='all':
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{ mode }}" in
        all|single) ;;
        *)
            echo "Invalid draw mode: '{{ mode }}' (expected: all|single)" >&2
            exit 1
            ;;
    esac

    # parse…
    keymap -c "{{ draw }}/config.yaml" \
      parse -z "{{ config }}/glove80.keymap" \
      --virtual-layers Combos \
      > "{{ draw }}/base.yaml"

    # fix layer names…
    yq -Yi '.combos.[].l = ["Combos"]' "{{ draw }}/base.yaml"

    # draw (uses physical_layout: glove80 from config.yaml)
    keymap -c "{{ draw }}/config.yaml" \
      draw "{{ draw }}/base.yaml" \
      > "{{ draw }}/base.svg"

    if [[ "{{ mode }}" == "single" ]]; then
        exit 0
    fi

    # also draw one SVG per layer
    layers_dir="{{ draw }}/layers"
    mkdir -p "$layers_dir"
    find "$layers_dir" -maxdepth 1 -type f -name '*.svg' -delete
    i=0
    yq -r '.layers | to_entries[] | .key' "{{ draw }}/base.yaml" | while IFS= read -r layer; do
        i=$((i + 1))
        safe_layer=$(printf '%s' "$layer" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/-/g; s/^-+|-+$//g')
        [[ -z "$safe_layer" ]] && safe_layer="layer-$i"
        layer_file="$layers_dir/$(printf '%02d' "$i")-$safe_layer.svg"

        keymap -c "{{ draw }}/config.yaml" \
          draw "{{ draw }}/base.yaml" \
          --select-layers "$layer" \
          --output "$layer_file"
    done

# initialize west
init:
    west init -l config
    west update --fetch-opt=--filter=blob:none
    west zephyr-export

# list build targets
list:
    @just _parse_targets all | sed 's/,*$//' | sort | column

# update west
update:
    west update --fetch-opt=--filter=blob:none

# upgrade zephyr-sdk and python dependencies
upgrade-sdk:
    nix flake update --flake .

[no-cd]
test $testpath *FLAGS:
    #!/usr/bin/env bash
    set -euo pipefail
    testcase=$(basename "$testpath")
    build_dir="{{ build / "tests" / '$testcase' }}"
    config_dir="{{ '$(pwd)' / '$testpath' }}"
    cd {{ justfile_directory() }}

    if [[ "{{ FLAGS }}" != *"--no-build"* ]]; then
        echo "Running $testcase..."
        rm -rf "$build_dir"
        west build -s zmk/app -d "$build_dir" -b native_posix_64 -- \
            -DCONFIG_ASSERT=y -DZMK_CONFIG="$config_dir"
    fi

    ${build_dir}/zephyr/zmk.exe | sed -e "s/.*> //" |
        tee ${build_dir}/keycode_events.full.log |
        sed -n -f ${config_dir}/events.patterns > ${build_dir}/keycode_events.log
    if [[ "{{ FLAGS }}" == *"--verbose"* ]]; then
        cat ${build_dir}/keycode_events.log
    fi

    if [[ "{{ FLAGS }}" == *"--auto-accept"* ]]; then
        cp ${build_dir}/keycode_events.log ${config_dir}/keycode_events.snapshot
    fi
    diff -auZ ${config_dir}/keycode_events.snapshot ${build_dir}/keycode_events.log
