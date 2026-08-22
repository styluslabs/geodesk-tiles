#!/usr/bin/env bash
set -euo pipefail

# geodesk-tiles setup script

#RUN_SERVER="/usr/bin/tmux new-session -d -s server 'cd $HOME/maps && ./geodesk-tiles/scripts/runsrv.sh geodesk-tiles/build/Release/server --threads 4 planet.gol ocean.gol'"

ARG1="${1:?Usage: $0 <server-name>}"

if [ ! -d "$HOME/maps" ]; then
  # install dependencies
  sudo apt update
  sudo apt install -y git curl wget unzip make g++ rsync nginx goaccess
  # nftables caddy

  # set hostname
  TARGET_HOSTNAME="${1:-}"
  SHORT_NAME=$(echo "$TARGET_HOSTNAME" | cut -d'.' -f1)
  sudo sed -i "/127.0.0.1/a 127.0.0.1   $TARGET_HOSTNAME $SHORT_NAME" /etc/hosts
  sudo hostnamectl set-hostname "$TARGET_HOSTNAME"

  # redirect 80 to 8080 ... handle w/ nginx for now (so cert challenge); clients can still use 8080 to bypass nginx
  #sudo systemctl enable nftables
  #sudo systemctl restart nftables
  #sudo nft add table ip nat
  #sudo nft add chain ip nat prerouting { type nat hook prerouting priority 0 \; }
  #sudo nft add rule ip nat prerouting tcp dport 80 redirect to :8080
  #sudo nft list ruleset > /etc/nftables.conf

  mkdir -p $HOME/maps
  cd $HOME/maps
  git clone https://github.com/styluslabs/geodesk-tiles.git

  # does not need to be updated
  wget -O ocean.gol https://github.com/styluslabs/geodesk-tiles/releases/download/tag-for-assets/ocean.gol

  # run server at reboot ... use actual service instead
  #(crontab -l 2>/dev/null; echo "@reboot $RUN_SERVER") | crontab -

  # unquoted EOF to expand ${} (use 'EOF' to disable expansion)
  sudo tee /etc/systemd/system/geodesk-tiles.service <<EOF
[Unit]
Description=geodesk-tiles tile and search server
After=network.target

[Service]
Type=simple
ExecStart=${HOME}/maps/geodesk-tiles/build/Release/server --threads 4 planet.gol ocean.gol
WorkingDirectory=${HOME}/maps
User=${USER}
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

  # enable running at startup
  sudo systemctl daemon-reload
  sudo systemctl enable geodesk-tiles

  # dummy certs so nginx will start
  sudo mkdir -p /etc/nginx/ssl
  sudo openssl req -x509 -nodes -newkey rsa:2048 -days 1 \
    -keyout /etc/nginx/ssl/tiles.styluslabs.com.key.pem \
    -out /etc/nginx/ssl/tiles.styluslabs.com.cert.pem \
    -subj "/CN=tiles.styluslabs.com"

  # nginx setup
  sudo ln -s $HOME/maps/geodesk-tiles/scripts/nginx.conf /etc/nginx/conf.d/tiles-styluslabs-com.conf
  sudo systemctl restart nginx

  # tiles-a should be set up last so reloadcmd works normally
  if [[ "$TARGET_HOSTNAME" == *"tiles-a.styluslabs.com"* ]]; then
    sudo mkdir -p /var/www/tiles.styluslabs.com
    sudo chown -R $USER:$USER /var/www/tiles.styluslabs.com
    chmod 755 $HOME/maps/geodesk-tiles/scripts/deploy_certs.sh
    # TLS setup
    git clone --depth 1 https://github.com/acmesh-official/acme.sh.git
    (cd acme.sh && ./acme.sh --install --accountemail "support@styluslabs.com")

    # -w folder should be served at /.well-known/acme-challenge/
    $HOME/.acme.sh/acme.sh --issue -d tiles.styluslabs.com -w /var/www/tiles.styluslabs.com
    $HOME/.acme.sh/acme.sh --install-cert -d tiles.styluslabs.com \
        --key-file $HOME/maps/tiles.styluslabs.com.key.pem  \
        --fullchain-file $HOME/maps/tiles.styluslabs.com.cert.pem \
        --reloadcmd "$HOME/maps/geodesk-tiles/scripts/deploy_certs.sh"
  fi
fi

cd $HOME/maps
(cd geodesk-tiles && git pull && git submodule update --init && rm -rf build/Release/server && make) &
# axel, aria2, or rclone for faster download?
#wget -O planet.gob https://download.openplanetdata.com/osm/planet/gob/v1/planet-latest.osm.gob
#gol load planet
#rm planet.gob
# need to see if this can recover from broken connections:
#gol load newplanet https://download.openplanetdata.com/osm/planet/gob/v1/planet-latest.osm.gob -C 8
#mv -f newplanet.gol planet.gol
wget -O planet.gol https://download.openplanetdata.com/osm/planet/gol/v1/planet-latest.osm.gol &
wait

# build search index
mv fts.sqlite fts.sqlite.old || true
geodesk-tiles/build/Release/server --buildfts 1 planet.gol ocean.gol
rm -f fts.sqlite.old

#$RUN_SERVER
# consider stopping existing server before downloading new planet.gol if disk space is a concern
sudo systemctl restart geodesk-tiles
echo "Setup completed; server started"
