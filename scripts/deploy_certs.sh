#!/bin/bash
set -euo pipefail

REMOTE_SERVERS=("tiles-b.styluslabs.com")
CERT_PATH="/etc/nginx/ssl"

echo "Installing certs locally..."
sudo mkdir -p $CERT_PATH
#sudo cp $HOME/.acme.sh/tiles.styluslabs.com_ecc/fullchain.cer $CERT_PATH/tiles.styluslabs.com.fullchain.pem
#sudo cp $HOME/.acme.sh/tiles.styluslabs.com_ecc/tiles.styluslabs.com.key $CERT_PATH/tiles.styluslabs.com.key.pem
sudo cp -f $HOME/maps/tiles.styluslabs.com.*.pem $CERT_PATH
sudo systemctl reload nginx

for server in "${REMOTE_SERVERS[@]}"; do
    echo "Deploying TLS certs to $server..."
    rsync --mkpath --rsync-path="sudo rsync" $HOME/maps/tiles.styluslabs.com.*.pem $USER@$server:$CERT_PATH/
    # Reload remote Nginx (reload tells service to just reload its config instead of completely restarting, if supported)
    ssh $server "sudo systemctl reload nginx"
done

echo "TLS cert deployments completed"
