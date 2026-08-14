# NIM connection-test TLS context

## Scope

This note records a firmware `20.5.0` NIM connection-test experiment on 2026-08-12.

The controlled endpoint patch was confirmed by a fresh device DNS observation for the replacement hostname.

The listener then received a TLS connection but no HTTP request.

## Finding

NIM and qLaunch both contain the same libcurl-based Horizon TLS backend.

NIM's connection-test time request explicitly requires peer and hostname verification.

Before the TLS transfer, its libcurl SSL-context callback creates a special system SSL context and registers the device's built-in client certificate with that context.

The installed sys-patch NIM crashfix disables that callback.

It therefore does not discard the connection test, but it removes the connection test's device-certificate context setup.

The shared TLS support has a system-context path that prefers the privileged `ssl:s` service and can fall back to ordinary `ssl`.

NIM is permitted to use `ssl:s` while qLaunch is permitted only to use `ssl`.

This is a concrete mechanism for the two processes to obtain distinct Horizon TLS contexts despite sharing the same libcurl implementation.

## Interpretation

The observed NIM TLS failure is not a URL-patch loading failure.

The current passive `ssl:s` observer accepted NIM's system SSL session during the same boot that resolved the controlled ctest hostname.

Its log omits the `DoHandshakeGetServerCert` operation used by NIM's backend.

The same log also omits the corresponding Account handshake despite successful Account DAuth traffic.

The absence of NIM connection-operation lines is therefore not evidence that the ctest transaction bypasses the public SSL connection object or fails before creating one.

With the crashfix disabled, NIM reaches that callback and aborts when `RegisterInternalPki(DeviceClientCertDefault)` returns an error.

The listener's resulting connection reset is therefore the process termination, not a completed TLS certificate verification failure.

The installed NIM crashfix bypasses this failure by removing the callback, but the resulting transfer still reaches the listener only to end with a pre-HTTP TLS EOF.

The evidence does not yet establish distinct trust stores, certificate pinning, TLS versions, cipher policy, or client-certificate requirements.

Those remain competing explanations within a potentially different system TLS context.

## Status

This path is deferred.

The observation does not currently change the Horizon networking-integration model.

If resumed, the next controlled run is a clean boot with the crashfix disabled, SSL MITM disabled, the URL patch retained, and PRODINFO available.
