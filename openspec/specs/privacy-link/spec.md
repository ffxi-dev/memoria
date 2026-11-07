# privacy-link Specification

## Purpose
Tells a player who links from the game what uploads, and points them at the privacy policy Mogsafe asks them to accept.

## Requirements

### Requirement: The first link names the privacy policy
The first `/memoria link` in a session SHALL print one chat line with the URL of Mogsafe's privacy policy, `<server>/privacy`, before the link code. Later links in the same session SHALL NOT print it again. The line SHALL NOT hold up the link; Mogsafe decides whether it goes ahead.

#### Scenario: First link
- **WHEN** a player runs `/memoria link` for the first time since the game started
- **THEN** chat shows the policy's URL, then the link code

#### Scenario: Second link
- **WHEN** the same player runs `/memoria link` again in that session
- **THEN** chat shows only the link code

### Requirement: Each link says what uploads
Every `/memoria link` SHALL first print "Memoria uploads everything you play on this install while linked, not only your /caps. Anyone can watch it, replay it and download it." Once linked, chat SHALL say "Linked to <account>. Your timeline: <server>/timeline/<world>/<character>", leaving out the timeline before the world is known.

#### Scenario: Linking in game
- **WHEN** a player on Asura as Kalnor runs `/memoria link` and completes it
- **THEN** chat shows that sentence, then the link code, and later "Linked to <account>. Your timeline: <server>/timeline/Asura/Kalnor"
