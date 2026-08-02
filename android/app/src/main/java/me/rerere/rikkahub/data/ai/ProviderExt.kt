package me.rerere.rikkahub.data.ai

import me.rerere.ai.provider.ProviderSetting

/** [CE] ProviderSetting 具体子类字段提取（baseUrl/apiKey 在 OpenAI/Google/Claude 子类） */
fun ProviderSetting.baseUrlOr(): String = when (this) {
    is ProviderSetting.OpenAI -> baseUrl
    is ProviderSetting.Google -> baseUrl
    is ProviderSetting.Claude -> baseUrl
    else -> ""
}

fun ProviderSetting.apiKeyOr(): String = when (this) {
    is ProviderSetting.OpenAI -> apiKey
    is ProviderSetting.Google -> apiKey
    is ProviderSetting.Claude -> apiKey
    else -> ""
}
