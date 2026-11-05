# Spec Delta

## Purpose

Points a player who links from the game at the privacy policy Mogsafe asks them to accept.

## ADDED Requirements

### Requirement: The first link names the privacy policy
The first `/memoria link` in a session SHALL print one chat line with the URL of Mogsafe's privacy policy, `<server>/privacy`, before the link code. Later links in the same session SHALL NOT print it again. The line SHALL gate nothing: Mogsafe decides whether a link goes ahead.

#### Scenario: First link
- **WHEN** a player runs `/memoria link` for the first time since the game started
- **THEN** chat shows the policy's URL, then the link code

#### Scenario: Second link
- **WHEN** the same player runs `/memoria link` again in that session
- **THEN** chat shows only the link code
