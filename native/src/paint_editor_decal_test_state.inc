// Included inside PaintEditor::Impl. Keep the synthetic pointer isolated from
// normal hover input whenever decalDiagnosticStage is nonzero.
int decalDiagnosticStage = 0;
Json decalDiagnosticResult;
PaintSnapshot decalDiagnosticBefore, decalDiagnosticFirst, decalDiagnosticAfter;
PaintStrokePoint decalDiagnosticFirstPoint, decalDiagnosticSecondPoint;
Camera decalDiagnosticCamera;
SurfaceDecalOptions decalDiagnosticPlacement;
uint64_t decalDiagnosticDepthUpdates = 0, decalDiagnosticUploads = 0, decalDiagnosticEvaluations = 0;
