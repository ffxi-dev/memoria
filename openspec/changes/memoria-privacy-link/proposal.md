# Proposal

## Why

The owner decided on 2026-11-03 that people accept a privacy policy before they link Memoria, import, or upload to Mogsafe (`ffxi-dev/vault`'s `openspec/changes/add-privacy-policy/`). Mogsafe enforces it: its link page shows the policy until the account agrees. A player who links from the game should learn where the policy lives before the browser opens.

## What Changes

- The first `/memoria link` of a session prints one chat line with the policy's URL, `<server>/privacy`, before it asks for a code. Later links in the same session print nothing more.

The line informs; it gates nothing. Mogsafe refuses a link the account has not agreed to, whatever Memoria prints.

## Capabilities

### New Capabilities
- `privacy-link`: `/memoria link` names the privacy policy.

### Modified Capabilities

## Impact

- **Memoria:** `src/plugin/session.*`.
- **Mogsafe:** serves `/privacy` and the gate, in vault's `add-privacy-policy`.
