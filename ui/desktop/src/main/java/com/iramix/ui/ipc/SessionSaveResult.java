package com.iramix.ui.ipc;

public record SessionSaveResult(
    long revision,
    long serializedBytes,
    long serializationNanoseconds,
    long durableSaveNanoseconds
) {}
