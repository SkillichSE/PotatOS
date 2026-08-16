from . import config
from . import models

def ask_llm(user_text, system_prompt):
    response = models.llm_client.chat.completions.create(
        model=config.lm_studio_model,
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_text},
        ],
        temperature=0.6,
        max_tokens=90,
    )
    return response.choices[0].message.content

def truncate_reply(text):
    text = text.strip()
    if len(text) <= config.llm_reply_max_chars:
        return text

    truncated = text[:config.llm_reply_max_chars]
    for punct in (".", "!", "?", "…"):
        idx = truncated.rfind(punct)
        if idx > 0:
            return truncated[:idx + 1]

    idx = truncated.rfind(" ")
    return truncated[:idx] if idx > 0 else truncated

def needs_internet_lookup(command):
    if not (config.internet_access_enabled and models.internet_search_available):
        return False

    if len(command.split()) < 2:
        return False

    lowered = command.lower()
    if any(kw in lowered for kw in config.internet_trigger_keywords):
        return True

    try:
        verdict = models.llm_client.chat.completions.create(
            model=config.lm_studio_model,
            messages=[
                {"role": "system", "content": config.normalizer_system_prompt},
                {"role": "user", "content": command},
            ],
            temperature=0.0,
            max_tokens=3,
        ).choices[0].message.content.strip().lower()
    except Exception as e:
        print(f"[internet] normalizer call failed: {e!r}")
        return False

    return verdict.startswith("да")

def search_web(query, max_results=None):
    if max_results is None:
        max_results = config.internet_max_results
    if not (config.internet_access_enabled and models.internet_search_available):
        return None
    try:
        with models.DDGS(timeout=config.internet_search_timeout) as ddgs:
            results = list(ddgs.text(query, region="ru-ru", max_results=max_results))
    except Exception as e:
        print(f"[internet] search failed: {e!r}")
        return None

    lines = []
    for r in results:
        title = (r.get("title") or "").strip()
        body = (r.get("body") or "").strip()
        if title and body:
            lines.append(f"- {title}: {body}")
        elif body:
            lines.append(f"- {body}")
    return "\n".join(lines) if lines else None

def build_effective_system_prompt(base_system_prompt, search_context):
    if not search_context:
        return base_system_prompt
    return (
        f"{base_system_prompt}\n\n"
        "Свежие результаты поиска в интернете по запросу пользователя (используй их как "
        "фактическую основу ответа, но перескажи своими словами в своём характере, коротко, "
        "не зачитывай список и не упоминай, что это результаты поиска):\n"
        f"{search_context}"
    )
