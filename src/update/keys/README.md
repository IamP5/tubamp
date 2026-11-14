# Trusted release keys

Every `*.pub.pem` file in this folder is compiled into the plugin as a key that may sign
updates. Nothing else is trusted. The folder is empty until the maintainer creates the
keys, and a build with `-DTUBAMP_UPDATES=ON` refuses to configure until it holds at least
two.

## Add the keys

You need two P-256 keys: the release key, which signs every release from CI, and the
recovery key, which stays offline and replaces the release key if it is ever lost.

```sh
openssl ecparam -name prime256v1 -genkey -noout -out release.pem
openssl ec -in release.pem -pubout -out src/update/keys/release.pub.pem
openssl ecparam -name prime256v1 -genkey -noout -out recovery.pem
openssl ec -in recovery.pem -pubout -out src/update/keys/recovery.pub.pem
```

Commit the two `.pub.pem` files through a normal pull request. Encrypt the two private
files, keep the recovery key in two offline places, and delete the plaintext copies. The
private files never enter the repository.

## What CMake refuses

Configuring fails when a file here holds a private key, is not a PEM public key, carries
the `TEST KEY` marker, or is one of the test keys in `tests/update/fixtures`. With
`TUBAMP_UPDATES=ON` it also fails with fewer than two keys.

## Check a key

```sh
openssl ec -pubin -in src/update/keys/release.pub.pem -outform DER | tail -c 65 | shasum -a 256 | cut -c1-16
```

prints the key id that appears in the `sig` lines of a signed `update-v1.signed`.
