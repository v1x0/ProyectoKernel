#!/bin/bash

if [ $(id -u $USER) -eq 0 ]; then
        if [ -f "./pwm" ]; then
                cp ./pwm /bin/
		chmod +x /bin/pwm
        else
                echo "Error, no se encuentra el archivo"
                return -1
        fi
        if [ -f "./pwm.ko" ]; then
                rmmod ./pwm.ko
                insmod ./pwm.ko
        else
                echo "Error, no se encuentra el kernel compilado"
                return -2
        fi
fi
