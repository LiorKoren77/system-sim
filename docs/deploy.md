# Deploying the web app

The live site is <https://syssim.stackchameleon.com>. It is **static files**: every scenario's day is
precomputed ahead of time, so the server never runs the simulator and needs no build tools. That is
why a small, mostly idle machine is enough.

```
this computer                                   the server (a small Ubuntu machine)
  npm run precompute  -> datasets
  npm run build       -> web/app/dist/   rsync   /var/www/syssim/   <- nginx, 127.0.0.1:8081
                                      ───────────>                         ^
                                                 Cloudflare Tunnel: syssim.<domain> -> localhost:8081
```

## Deploying a new version

```bash
cmake --preset gcc14-release && cmake --build --preset gcc14-release   # once, for the simulator
deploy/deploy.sh --dry-run        # build, then list what would change on the server
deploy/deploy.sh                  # build and copy
deploy/deploy.sh --skip-precompute   # only the page changed, not the models: skips the 3 minutes
```

The script reads `SYSSIM_HOST` (an `ssh` alias, default `lightsail`), `SYSSIM_DEST` (default
`/var/www/syssim`) and `SIMSYS` (the Release binary). If the page still shows old data afterwards,
purge Cloudflare's cache (the domain's *Caching* → *Configuration* → *Purge Everything*).

## One-time setup

**1. Reaching the server.** The machine has only an IPv6 address, which many home networks cannot
reach, so `ssh` goes through the machine's Cloudflare Tunnel. Install `cloudflared` locally and add
to `~/.ssh/config`:

```
Host lightsail
  HostName <the tunnel's SSH hostname>
  User ubuntu
  IdentityFile ~/.ssh/<your private key>
  ProxyCommand cloudflared access ssh --hostname %h
```

The tunnel's SSH hostname needs an Access policy that allows your email. Your public key goes on the
server in `~/.ssh/authorized_keys` (append with `>>`, so other keys stay). The cloud console's
browser terminal can do that when no key works yet.

**2. The folder and the web server**, on the server:

```bash
sudo mkdir -p /var/www/syssim && sudo chown ubuntu:ubuntu /var/www/syssim
sudo tee /etc/nginx/sites-available/syssim.conf >/dev/null <<'EOF'
server {
    listen 127.0.0.1:8081;     # localhost only: the tunnel is the only way in
    root /var/www/syssim;
    index index.html;
    location / { try_files $uri $uri/ =404; }
}
EOF
sudo ln -s /etc/nginx/sites-available/syssim.conf /etc/nginx/sites-enabled/syssim.conf
sudo nginx -t && sudo systemctl reload nginx     # reload only if the test passes
curl -s http://127.0.0.1:8081/ | head -3
```

**3. The domain.** The domain's nameservers are Cloudflare's (set at the registrar), and the
domain is active in the same Cloudflare account as the tunnel. Delete any placeholder DNS records the
registrar's parking page left (a wildcard `*` record sends every name to a stranger's server). Then
in the tunnel's *Public hostname* add `syssim` on the domain, type `HTTP`, URL `localhost:8081`.
Cloudflare creates the DNS record itself.

## If something is wrong

| Symptom | Likely cause |
|---|---|
| `502 Bad Gateway` | Nothing answers on the server's port 8081: run the `curl` above on the server |
| Error `1033` or `1016` | The tunnel or the DNS record is not recognised yet (wait a few minutes), or the hostname row was not saved |
| `Permission denied (publickey)` | The public key is missing or broken in the server's `authorized_keys` |
| A blank page or an empty scenario list | The datasets did not copy: look at the browser's Network tab, then run `deploy/deploy.sh --dry-run` |
| `ssh` says the network is unreachable | The `ProxyCommand` line is missing: the server has no IPv4 address and this computer no IPv6 |

## What is not covered

The planned design-challenge mode needs the simulator to run for each visitor's design, which a
static server cannot do. It will need its own deployment: a Linux build of `simsys` on the server and
limits on how much one visitor can use. Until then, the live site is the watch-only app.
