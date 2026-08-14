# BaaS Account Bootstrap

## Scope

This note records the Account module's initial local-BaaS bootstrap on firmware `20.5.0`.

The underlying binary evidence and decompiler output remain in the firmware-local private notes.

## Observed and static flow

After obtaining a BaaS application token, Account sends a bodyless POST request to create a user resource.

The client requires a successful HTTP status and a syntactically valid JSON response before it can proceed to the next account-flow step.

An empty response is not sufficient.

## Current implementation consequence

A local replacement should persist one stable user identity per authenticated device.

The creation-response adapter reads `deviceAccounts[0].id` and `deviceAccounts[0].password`.

The subsequent Account request uses the returned identifier as a fixed-width hexadecimal user ID and forwards the password through a field bounded to 40 characters.

The [Pretendo BaaS reference](https://nintendo-wiki.pretendo.network/docs/switch/baas.html) documents the device-account identifier as a 16-character hexadecimal JSON string and the password as a 40-character alphanumeric string.

A local `200` response containing a numeric identifier caused Account error `2124-3121`, which confirms that the client rejected the successful HTTP response as invalid.

User creation is documented to return `201 Created` and a user-resource envelope.

The static parser references the documented `deviceAccounts[0].id` and `deviceAccounts[0].password` members.

The local listener now returns that provisional `201` resource shape with separately persisted account and device-account identifiers.

The observed next request is `POST /1.0.0/login` with a form-encoded device-account ID, password, `naCountry=Unknown`, and `isPersistent=false`.

The static login-response adapter reads `user.id`, `expiresIn`, `accessToken`, and `idToken`.

The login response requires all four of those fields before Account accepts it.

`user.id` is interpreted as a fixed-width hexadecimal account identifier.

The Account login flow without `appAuthNToken` accepts an absent Nintendo Account link and an absent membership summary.

Account compares the returned user identifier with its persisted local BaaS-user identifier before storing the new session state.

The immediate response path stores the access token for later use and does not perform an apparent token-signature verification.

## Account and NPNS relationship

Account provides the device-authentication service consumed by NPNS and also consumes the NPNS system-service surface.

NPNS independently contacts its God/Penne push-service endpoint and has request forms that carry an account identity token.

The auth-only resolver trace interleaves Account's DAuth and BaaS resolutions with NPNS's God/Penne resolution.

This establishes a shared authentication and notification dependency but does not by itself show that a BaaS login response triggered a particular NPNS request.

Until BaaS token records are durable, the listener can temporarily associate an unmatched cached bearer with its SHA-256 digest rather than reject it.

This is a compatibility fallback, not device authentication.

## Next questions

Confirm that the listener returns the same BaaS user identifier after restart and across cached-bearer recovery.

Trace the first request that consumes the stored access token.
