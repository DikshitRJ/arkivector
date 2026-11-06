# `daemon/src/server/auth.h`

Declares API key authentication and RBAC scope authorization interfaces for the HTTP server.

## Overview
Defines capabilities, scopes (`AV_AUTH_READ`, `AV_AUTH_WRITE`, `AV_AUTH_ADMIN`), and functions for:
- Checking whether auth is enabled (`av_auth_is_enabled`).
- Determining if a request path is unauthenticated (`/healthz`, `/readyz`, `/v1/version`, `/metrics`).
- Evaluating the required scope for a given method and path (`av_auth_required_scope`).
- Verifying incoming Bearer tokens using constant-time SHA-256 comparison (`av_auth_check`).
- Lifecycle management (`av_auth_create`, `av_auth_destroy`).
