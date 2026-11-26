---
status: accepted
---

# Zadania przetwarzania żyją niezależnie od okna i asystenta

Obliczenia HDMapping mogą trwać długo. Użytkownik wybrał kontynuację zadań po zamknięciu asystenta, rozłączeniu MCP oraz zamknięciu GUI (Q7, Q11), a także równoległe wykonywanie kilku zadań (Q12). Zamknięcie interfejsu nie oznacza anulowania obliczeń; ponowne połączenie umożliwia odczyt postępu i wyników.

Q20 obejmuje tym samym modelem ciężkie obliczenia uruchamiane przyciskami aktualnych GUI. Przy włączonej integracji koordynator działa także bez asystenta; źródło polecenia nie zmienia limitu i trwałości zadania.

Wariant związania zadania z życiem wywołania MCP lub otwartego okna jest prostszy w integracji z istniejącymi aplikacjami, ale nie spełnia tych wymagań. Konsekwencją decyzji jest odrębne zarządzanie wykonaniem i identyfikacją zadań. Q13 ustala domyślny limit 2 zadań i kolejkę; Q16 ustala anulowanie oraz ponowne wykonanie od początku po awarii, bez wznawiania od iteracji. Architekturę techniczną opisuje ADR 0002.
