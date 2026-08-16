from . import config

def has_repetition(text, min_repeats=3):
    words = text.lower().split()
    if len(words) < min_repeats * 2:
        return False

    for phrase_len in (1, 2, 3):
        for i in range(len(words) - phrase_len * min_repeats + 1):
            phrase = words[i:i + phrase_len]
            repeats = 1
            j = i + phrase_len
            while j + phrase_len <= len(words) and words[j:j + phrase_len] == phrase:
                repeats += 1
                j += phrase_len
            if repeats >= min_repeats:
                return True
    return False

def is_known_hallucination_phrase(text):
    normalized = text.strip(" .!?…").lower()
    return normalized in config.known_hallucination_phrases

def is_hallucination(text):
    return has_repetition(text) or is_known_hallucination_phrase(text)

def _levenshtein(a, b):
    if a == b:
        return 0
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev_row = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur_row = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cost = 0 if ca == cb else 1
            cur_row[j] = min(
                prev_row[j] + 1,
                cur_row[j - 1] + 1,
                prev_row[j - 1] + cost,
            )
        prev_row = cur_row
    return prev_row[-1]

_phonetic_pairs = {
    'б': 'п', 'в': 'ф', 'г': 'к', 'д': 'т', 'ж': 'ш', 'з': 'с',
    'о': 'а', 'ё': 'е', 'и': 'е', 'ы': 'и',
}

def _phonetic_key(word):
    mapped = "".join(_phonetic_pairs.get(ch, ch) for ch in word.lower())
    collapsed = []
    for ch in mapped:
        if not collapsed or collapsed[-1] != ch:
            collapsed.append(ch)
    return "".join(collapsed)

_wake_key = _phonetic_key(config.wake_word_canonical)
_wake_variant_keys = {_phonetic_key(v) for v in config.wake_word_known_variants} | {_wake_key}

def is_close_to_wake_word(word):
    word = word.strip(".,!?—-\"'")
    if not word:
        return False
    if word in config.wake_word_known_variants:
        return True

    key = _phonetic_key(word)
    if key in _wake_variant_keys:
        return True

    max_dist = 1 if len(_wake_key) <= 5 else 2
    return _levenshtein(key, _wake_key) <= max_dist

_module_command_words = {"модуль", "модули", "модуля", "модулю"}

def check_module_command(command):
    words = command.lower().strip(" .,!?—-").split()
    if not words or words[0] not in _module_command_words:
        return None

    rest = " ".join(words[1:]).strip(" .,!?—-")
    if not rest:
        return None

    for module_id, names in config.glados_module_spoken_names.items():
        for name in names:
            if rest == name or rest.startswith(name + " ") or name in rest:
                return module_id
    return None

def check_wake_word(text):
    words = text.lower().split()
    for i, w in enumerate(words):
        if is_close_to_wake_word(w):
            rest = words[i + 1:]
        elif i + 1 < len(words) and is_close_to_wake_word(w + words[i + 1]):
            rest = words[i + 2:]
        else:
            continue

        while rest and is_close_to_wake_word(rest[0]):
            rest = rest[1:]

        return " ".join(rest).strip()
    return None
