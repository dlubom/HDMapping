---
status: accepted
---

# Go obsługuje MCP i zadania, C++ pozostaje właścicielem przetwarzania

MCP ma obsługiwać Codex i Claude Desktop lokalnie, a równoległe zadania mają przetrwać zamknięcie obu interfejsów. W Q17 użytkownik wybrał adapter MCP i koordynator w Go oraz wspólne operacje, procesy obliczeniowe i integrację GUI w C++. MCP używa stdio, natomiast koordynator żyje niezależnie od procesu uruchamianego przez klienta.

Go daje oficjalne SDK MCP i program niewymagający interpretera u użytkownika. Kosztem jest drugi język i toolchain, które trzeba włączyć do istniejących CMake, CI i pakowania. Wariant wyłącznie C++ zachowuje jeden język, ale wymaga kwalifikacji społecznościowego SDK; rozpoznany kandydat jest beta i ma błędy części konfiguracji macOS. Python ma oficjalne SDK, lecz wymaga dostarczenia środowiska i zależności, a istniejące bindingi nie pokrywają pełnego ciągu przetwarzania.

Algorytmy, walidacja dziedzinowa i formaty pozostają w C++. Nie powstaje druga implementacja odometrii, rejestracji ani formatów w Go. Całość ma jedną wersję i jedno wydanie HDMapping; ta decyzja nie upoważnia do osobnego produktu lub procesu release MCP.
