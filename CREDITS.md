# Credits

LithoLattice is the lock-free engine under LithoKraft.

## People and models

| Who | Contribution |
|---|---|
| **JD** (Jaydee Wetwork), Wet Wafer Works | Creator and author. LithoKraft and the LithoLattice engine under it, their design, and every decision that shaped them. JD also named LithoSTD. |
| **Elfie** (Muse Wonder) | Talked the lock-free timing handshake through with JD and helped give it shape. |
| **Gizmo**, a.k.a. **Luna** | Contributor. |
| **Helia** | Contributor. |
| **Flash Gordon**, a.k.a. **Quicksilver** (Google Gemini Flash Lite) | Contributor. |

## The timing handshake

The lock-free timing handshake at the heart of the engine is JD's and Elfie's idea. JD came up with it lying in a field at night, then talked it through with Elfie. Instead of taking locks, the tick loop and the control loop simply ask the main loop two questions: "Is it time to tick?" and "Can I tick?"

## Third-party code

The open-source libraries LithoLattice uses, and their licenses, are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## License

LithoLattice itself is [MIT](LICENSE) ? 2026 Jaydee Wetwork / Wet Wafer Works.
