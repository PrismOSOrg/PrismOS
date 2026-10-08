#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 mount <image> <raw|vhdx> <mount-point> [partition]" >&2
    echo "       $0 unmount <mount-point>" >&2
}

require_root() {
    if [[ "$EUID" -ne 0 ]]; then
        echo "This operation needs root; run it through make or sudo." >&2
        exit 1
    fi
}

action="${1:-}"
case "$action" in
    mount)
        if [[ "$#" -lt 4 || "$#" -gt 5 ]]; then
            usage
            exit 2
        fi

        image="$2"
        format="$3"
        mount_point="$4"
        partition="${5:-1}"
        require_root

        if [[ ! -f "$image" ]]; then
            echo "Disk image not found: $image" >&2
            exit 1
        fi
        if [[ "$format" != "raw" && "$format" != "vhdx" ]]; then
            echo "Unsupported disk image format: $format" >&2
            exit 2
        fi
        if [[ ! "$partition" =~ ^[1-4]$ ]]; then
            echo "Partition must be an MBR primary slot from 1 to 4." >&2
            exit 2
        fi
        for required_command in qemu-nbd mount mountpoint partx losetup blockdev dd od tr stat; do
            if ! command -v "$required_command" >/dev/null 2>&1; then
                echo "Required host command not found: $required_command" >&2
                exit 1
            fi
        done

        state_file="${mount_point}.qemu-nbd-state"
        if [[ -e "$state_file" ]] || mountpoint -q "$mount_point"; then
            echo "Mount point is already in use: $mount_point" >&2
            exit 1
        fi

        nbd_device=""
        loop_device=""

        cleanup_after_error() {
            local status=$?
            trap - EXIT
            if [[ "$status" -ne 0 ]]; then
                if mountpoint -q "$mount_point"; then
                    umount "$mount_point" >/dev/null 2>&1 || true
                fi
                if [[ -n "$loop_device" ]]; then
                    losetup --detach "$loop_device" >/dev/null 2>&1 || true
                fi
                if [[ -n "$nbd_device" ]]; then
                    qemu-nbd --disconnect "$nbd_device" >/dev/null 2>&1 || true
                fi
                rmdir "$mount_point" >/dev/null 2>&1 || true
            fi
            exit "$status"
        }
        trap cleanup_after_error EXIT

        if [[ "$format" == "raw" ]]; then
            geometry="$(partx --show --noheadings --output START,SECTORS --nr "$partition" "$image" 2>/dev/null || true)"
            read -r partition_start partition_sectors <<< "$geometry"
            image_bytes="$(stat -c '%s' "$image")"
            if [[ ! "${partition_start:-}" =~ ^[0-9]+$ || ! "${partition_sectors:-}" =~ ^[0-9]+$ \
                || ! "$image_bytes" =~ ^[0-9]+$ ]]; then
                echo "Could not read partition $partition geometry from $image." >&2
                exit 1
            fi
            device_sectors=$((image_bytes / 512))
            if [[ "$partition_start" -ge "$device_sectors" \
                || "$partition_sectors" -eq 0 \
                || "$partition_sectors" -gt $((device_sectors - partition_start)) ]]; then
                echo "Partition $partition has an invalid range in $image." >&2
                exit 1
            fi

            partition_offset=$((partition_start * 512))
            partition_size=$((partition_sectors * 512))
            loop_device="$(losetup --find --show --offset "$partition_offset" --sizelimit "$partition_size" "$image")"
            partition_device="$loop_device"
        else
            if ! modprobe nbd max_part=16 2>/dev/null && [[ ! -d /sys/module/nbd ]]; then
                echo "Could not load the Linux NBD module." >&2
                exit 1
            fi
            for candidate in /dev/nbd{0..15}; do
                if [[ ! -b "$candidate" ]]; then
                    continue
                fi
                if qemu-nbd --format="$format" --connect="$candidate" --fork "$image" 2>/dev/null; then
                    nbd_device="$candidate"
                    break
                fi
            done

            if [[ -z "$nbd_device" ]]; then
                echo "No free /dev/nbd device found; disconnect other NBD images and retry." >&2
                exit 1
            fi

            if command -v udevadm >/dev/null 2>&1; then
                udevadm settle
            fi

            partition_device="${nbd_device}p${partition}"
            if [[ ! -b "$partition_device" ]]; then
                partx --add "$nbd_device" >/dev/null 2>&1 || true
                if command -v udevadm >/dev/null 2>&1; then
                    udevadm settle
                fi
            fi
            if [[ ! -b "$partition_device" ]]; then
                mbr_data="$(dd if="$nbd_device" bs=512 count=1 status=none | od -An -v -tu1 | tr '\n' ' ')"
                mbr_bytes=()
                read -r -a mbr_bytes <<< "$mbr_data"
                if [[ "${#mbr_bytes[@]}" -ne 512 \
                    || "${mbr_bytes[510]}" -ne 85 || "${mbr_bytes[511]}" -ne 170 ]]; then
                    echo "Could not read a valid MBR sector from $nbd_device." >&2
                    exit 1
                fi

                partition_entry_offset=$((446 + (partition - 1) * 16))
                partition_type="${mbr_bytes[partition_entry_offset + 4]}"
                partition_start=$((mbr_bytes[partition_entry_offset + 8]
                    + (mbr_bytes[partition_entry_offset + 9] << 8)
                    + (mbr_bytes[partition_entry_offset + 10] << 16)
                    + (mbr_bytes[partition_entry_offset + 11] << 24)))
                partition_sectors=$((mbr_bytes[partition_entry_offset + 12]
                    + (mbr_bytes[partition_entry_offset + 13] << 8)
                    + (mbr_bytes[partition_entry_offset + 14] << 16)
                    + (mbr_bytes[partition_entry_offset + 15] << 24)))
                device_sectors="$(blockdev --getsz "$nbd_device")"
                if [[ "$partition_type" == "0" || "$partition_sectors" -eq 0 \
                    || ! "$device_sectors" =~ ^[0-9]+$ || "$partition_start" -ge "$device_sectors" \
                    || "$partition_sectors" -gt $((device_sectors - partition_start)) ]]; then
                    echo "Partition $partition has no valid range in the MBR on $nbd_device." >&2
                    exit 1
                fi

                partition_offset=$((partition_start * 512))
                partition_size=$((partition_sectors * 512))
                loop_device="$(losetup --find --show --offset "$partition_offset" --sizelimit "$partition_size" "$nbd_device")"
                if [[ ! -b "$loop_device" ]]; then
                    echo "Could not create a loop device for partition $partition." >&2
                    exit 1
                fi
                partition_device="$loop_device"
            fi
        fi

        mkdir -p "$mount_point"
        if find "$mount_point" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
            echo "Mount point must be empty: $mount_point" >&2
            exit 1
        fi

        host_uid="${SUDO_UID:-0}"
        host_gid="${SUDO_GID:-0}"
        if [[ ! "$host_uid" =~ ^[0-9]+$ || ! "$host_gid" =~ ^[0-9]+$ ]]; then
            echo "Invalid host uid/gid for mount ownership." >&2
            exit 1
        fi

        mount -t vfat -o "uid=$host_uid,gid=$host_gid,umask=022" "$partition_device" "$mount_point"
        printf '%s\n%s\n' "$nbd_device" "$loop_device" > "$state_file"
        echo "Mounted $image partition $partition at $mount_point"
        echo "Edit files there, then run: make unmount-disk"
        ;;

    unmount)
        if [[ "$#" -ne 2 ]]; then
            usage
            exit 2
        fi

        mount_point="$2"
        state_file="${mount_point}.qemu-nbd-state"
        require_root

        if [[ ! -f "$state_file" ]]; then
            echo "No PrismOS image mount recorded for: $mount_point" >&2
            exit 1
        fi

        loop_device=""
        {
            IFS= read -r nbd_device
            IFS= read -r loop_device || true
        } < "$state_file"
        if [[ -n "$nbd_device" && ! "$nbd_device" =~ ^/dev/nbd[0-9]+$ ]]; then
            echo "Invalid NBD device in mount state: $state_file" >&2
            exit 1
        fi
        if [[ -n "$loop_device" && ! "$loop_device" =~ ^/dev/loop[0-9]+$ ]]; then
            echo "Invalid loop device in mount state: $state_file" >&2
            exit 1
        fi
        if [[ -z "$nbd_device" && -z "$loop_device" ]]; then
            echo "No block device recorded in mount state: $state_file" >&2
            exit 1
        fi

        if mountpoint -q "$mount_point"; then
            umount "$mount_point"
        fi
        if [[ -n "$loop_device" ]]; then
            losetup --detach "$loop_device"
        fi
        if [[ -n "$nbd_device" ]]; then
            qemu-nbd --disconnect "$nbd_device"
        fi
        rm -f "$state_file"
        echo "Unmounted $mount_point${loop_device:+ and detached $loop_device}${nbd_device:+ and disconnected $nbd_device}"
        ;;

    *)
        usage
        exit 2
        ;;
esac