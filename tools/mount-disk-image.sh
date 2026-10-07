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
        if [[ ! "$partition" =~ ^[1-9][0-9]*$ ]]; then
            echo "Partition must be a positive integer." >&2
            exit 2
        fi
        for required_command in qemu-nbd mount mountpoint partx; do
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

        if ! modprobe nbd max_part=16 2>/dev/null && [[ ! -d /sys/module/nbd ]]; then
            echo "Could not load the Linux NBD module." >&2
            exit 1
        fi
        nbd_device=""
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

        cleanup_after_error() {
            local status=$?
            trap - EXIT
            if [[ "$status" -ne 0 ]]; then
                if mountpoint -q "$mount_point"; then
                    umount "$mount_point" >/dev/null 2>&1 || true
                fi
                qemu-nbd --disconnect "$nbd_device" >/dev/null 2>&1 || true
                rmdir "$mount_point" >/dev/null 2>&1 || true
            fi
            exit "$status"
        }
        trap cleanup_after_error EXIT

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
            echo "Partition $partition was not exposed by $nbd_device." >&2
            exit 1
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
        printf '%s\n' "$nbd_device" > "$state_file"
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

        IFS= read -r nbd_device < "$state_file"
        if [[ ! "$nbd_device" =~ ^/dev/nbd[0-9]+$ ]]; then
            echo "Invalid NBD device in mount state: $state_file" >&2
            exit 1
        fi

        if mountpoint -q "$mount_point"; then
            umount "$mount_point"
        fi
        qemu-nbd --disconnect "$nbd_device"
        rm -f "$state_file"
        echo "Unmounted $mount_point and disconnected $nbd_device"
        ;;

    *)
        usage
        exit 2
        ;;
esac