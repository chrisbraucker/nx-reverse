# NPNS Penne service discovery

The NPNS system program independently resolves a God/Penne push-service host during an auth-only boot on firmware `20.5.0`.

Its executable contains a second Val/Penne URI template that has not yet been observed at runtime.

NPNS accepts device-authentication service input and has request forms that include an account identity token, notification-token data, and Penne credentials.

This supports a shared Account and NPNS authentication boundary without proving a direct BaaS-to-NPNS HTTP dependency.

The first controlled experiment should redirect only the observed God host and capture the request before attempting to implement the service.
