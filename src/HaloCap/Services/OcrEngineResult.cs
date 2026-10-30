using System.Collections.Generic;
using HaloCap.Models;

namespace HaloCap.Services;

internal sealed record OcrEngineResult(
    OcrStatus Status,
    IReadOnlyList<string> Lines,
    string Message,
    string EngineLabel);
