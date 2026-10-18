#!/bin/sh
# Run from the repository: sudo sh deploy/install-web.sh
set -eu
cd /home/aiam/opt/002demoCpp
backup="/etc/nginx/sites-available/imu.conf.before-web-download.$(date +%Y%m%d%H%M%S)"
cp -a /etc/nginx/sites-available/imu.conf "$backup"
install -m 644 deploy/imu.conf /etc/nginx/sites-available/imu.conf
if ! nginx -t; then
    cp -a "$backup" /etc/nginx/sites-available/imu.conf
    exit 1
fi
install -m 644 deploy/iot-download.service /etc/systemd/system/iot-download.service
systemctl daemon-reload
systemctl enable --now iot-download.service
systemctl restart iot-download.service
if systemctl is-active --quiet nginx; then
    systemctl reload nginx
else
    systemctl start nginx
fi
