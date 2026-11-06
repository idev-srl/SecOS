/*
 * dropbear_localoptions.h - SecOS build options for the Dropbear SSH port.
 * Copied to <dropbear>/localoptions.h by tools/port-dropbear.sh; overrides
 * src/default_options.h. See docs/SSH.md.
 * SPDX-License-Identifier: MIT
 *
 * Policy: every account in /etc/passwd may log in (multi-user, M42) with a
 * public key from ~user/.ssh/authorized_keys or with its password from
 * /etc/shadow (checked with the libc crypt(), user/libcrypt.a). Root may log
 * in with a key only: the SecOS build refuses root password logins (as if
 * -g were always given). After authentication the session process drops to
 * the user (setgid + initgroups + setuid). No forwarding of any kind, modern
 * algorithms only.
 */
#ifndef SECOS_DROPBEAR_LOCALOPTIONS_H
#define SECOS_DROPBEAR_LOCALOPTIONS_H

/* --- files --- */
#define ED25519_PRIV_FILENAME "/etc/dropbear/dropbear_ed25519_host_key"
#define DROPBEAR_PIDFILE "/tmp/dropbear.pid"
#define DEFAULT_PATH "/bin:/usr/bin"
#define DEFAULT_ROOT_PATH "/bin:/usr/bin:/sbin:/usr/sbin"

/* --- modes --- */
#define NON_INETD_MODE 1
#define INETD_MODE 1
/* Re-exec per connection needs /proc/self/exe + real execve; the fork child
 * keeps the parent's layout (SecOS has no ASLR yet anyway). */
#define DROPBEAR_REEXEC 0
#ifndef DEBUG_TRACE
#define DEBUG_TRACE 0
#endif
#define DROPBEAR_SMALL_CODE 0

/* --- no forwarding, no agent, no X11, no proxy helpers --- */
#define DROPBEAR_X11FWD 0
#define DROPBEAR_CLI_LOCALTCPFWD 0
#define DROPBEAR_CLI_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALTCPFWD 0
#define DROPBEAR_SVR_REMOTETCPFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_CLI_AGENTFWD 0
#define DROPBEAR_CLI_PROXYCMD 0
#define DROPBEAR_CLI_NETCAT 0
#define DROPBEAR_USER_ALGO_LIST 1

/* --- ciphers: chacha20-poly1305, aes-gcm, aes-ctr --- */
#define DROPBEAR_AES128 1
#define DROPBEAR_AES256 1
#define DROPBEAR_3DES 0
#define DROPBEAR_CHACHA20POLY1305 1
#define DROPBEAR_ENABLE_CTR_MODE 1
#define DROPBEAR_ENABLE_CBC_MODE 0
#define DROPBEAR_ENABLE_GCM_MODE 1

/* --- MACs: hmac-sha2 only --- */
#define DROPBEAR_SHA1_HMAC 0
#define DROPBEAR_SHA2_256_HMAC 1
#define DROPBEAR_SHA2_512_HMAC 1
#define DROPBEAR_SHA1_96_HMAC 0

/* --- host / user keys: ed25519 only --- */
#define DROPBEAR_RSA 0
#define DROPBEAR_RSA_SHA1 0
#define DROPBEAR_DSS 0
#define DROPBEAR_ECDSA 0
#define DROPBEAR_ED25519 1
#define DROPBEAR_SK_KEYS 0
/* -R: generate the host key on first use if it is missing. */
#define DROPBEAR_DELAY_HOSTKEY 1

/* --- key exchange: mlkem768x25519 (post-quantum hybrid) + curve25519 --- */
#define DROPBEAR_DH_GROUP14_SHA1 0
#define DROPBEAR_DH_GROUP14_SHA256 0
#define DROPBEAR_DH_GROUP16 0
#define DROPBEAR_CURVE25519 1
#define DROPBEAR_SNTRUP761 0
#define DROPBEAR_MLKEM768 1
#define DROPBEAR_ECDH 0
#define DROPBEAR_DH_GROUP1 0
#define DROPBEAR_DH_GROUP1_CLIENTONLY 0
#define DROPBEAR_CLI_COMPRESSION 0

/* --- authentication: public key, or password from /etc/shadow --- */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_PUBKEY_OPTIONS 1   /* honour command=/no-pty/... restrictions */
#define DROPBEAR_CLI_PASSWORD_AUTH 0
#define DROPBEAR_CLI_PUBKEY_AUTH 1
#define DROPBEAR_USE_PASSWORD_ENV 0
#define DROPBEAR_CLI_ASKPASS_HELPER 0
/* Multi-user: the server runs as root (init starts it) and serves any account;
 * the per-connection process switches to the user as soon as authentication
 * succeeds (DROP_PRIVS), so the channel, the pty and the shell never run as
 * root for a non-root login. */
#define DROPBEAR_SVR_MULTIUSER 1
#define DROPBEAR_SVR_DROP_PRIVS 1
/* Valid login shells when /etc/shells is missing (Dropbear's getusershell). */
#define COMPAT_USER_SHELLS "/bin/bash", "/bin/sh"

/* --- misc --- */
#define DO_HOST_LOOKUP 0
#define DO_MOTD 0
#define DROPBEAR_SFTPSERVER 0
#define DROPBEAR_USE_PRNGD 0
#define DROPBEAR_USE_SSH_CONFIG 0
#define MAX_AUTH_TRIES 6
#define UNAUTH_CLOSE_DELAY 0

#endif /* SECOS_DROPBEAR_LOCALOPTIONS_H */
