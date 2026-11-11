REM LibreOffice Calc: sort arrows in the header row of every elotto_*.csv.
REM Bound to the event "Dokument öffnen" (Extras > Anpassen > Ereignisse).
REM Every other document is left alone.

Sub ElottoAutoFilter(Optional oEvent As Object)
    Dim oDoc As Object, oSheet As Object, oCur As Object, oRanges As Object
    Dim sUrl As String

    On Error GoTo Done
    If IsMissing(oEvent) Then
        oDoc = ThisComponent
    Else
        oDoc = oEvent.Source
    End If
    If IsNull(oDoc) Then Exit Sub
    If Not oDoc.supportsService("com.sun.star.sheet.SpreadsheetDocument") Then Exit Sub

    sUrl = LCase(oDoc.getURL())
    If InStr(sUrl, "elotto_") = 0 Then Exit Sub
    If Right(sUrl, 4) <> ".csv" Then Exit Sub

    oSheet = oDoc.Sheets.getByIndex(0)
    oCur = oSheet.createCursor()
    oCur.gotoStartOfUsedArea(False)
    oCur.gotoEndOfUsedArea(True)

    oRanges = oDoc.DatabaseRanges
    If oRanges.hasByName("elotto") Then oRanges.removeByName("elotto")
    oRanges.addNewByName("elotto", oCur.getRangeAddress())
    oRanges.getByName("elotto").AutoFilter = True

    REM keep the header row visible while scrolling
    oDoc.getCurrentController().freezeAtPosition(0, 1)
Done:
End Sub
