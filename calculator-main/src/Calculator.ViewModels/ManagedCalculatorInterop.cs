using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace CalcManager.Interop
{
    public enum CalculatorCommand
    {
        CommandNULL = 0, CommandSIGN = 80, CommandCLEAR = 81, CommandCENTR = 82, CommandBACK = 83,
        CommandPNT = 84, CommandAnd = 86, CommandOR = 87, CommandXor = 88, CommandLSHF = 89,
        CommandRSHF = 90, CommandDIV = 91, CommandMUL = 92, CommandADD = 93, CommandSUB = 94,
        CommandMOD = 95, CommandROOT = 96, CommandPWR = 97, CommandCHOP = 98, CommandROL = 99,
        CommandROR = 100, CommandCOM = 101, CommandSIN = 102, CommandCOS = 103, CommandTAN = 104,
        CommandSINH = 105, CommandCOSH = 106, CommandTANH = 107, CommandLN = 108, CommandLOG = 109,
        CommandSQRT = 110, CommandSQR = 111, CommandCUB = 112, CommandFAC = 113, CommandREC = 114,
        CommandDMS = 115, CommandCUBEROOT = 116, CommandPOW10 = 117, CommandPERCENT = 118,
        CommandFE = 119, CommandPI = 120, CommandEQU = 121, CommandMCLEAR = 122, CommandRECALL = 123,
        CommandSTORE = 124, CommandMPLUS = 125, CommandMMINUS = 126, CommandEXP = 127,
        CommandOPENP = 128, CommandCLOSEP = 129, Command0 = 130, Command1 = 131, Command2 = 132,
        Command3 = 133, Command4 = 134, Command5 = 135, Command6 = 136, Command7 = 137,
        Command8 = 138, Command9 = 139, CommandA = 140, CommandB = 141, CommandC = 142,
        CommandD = 143, CommandE = 144, CommandF = 145, CommandINV = 146, CommandSET_RESULT = 147,
        ModeBasic = 200, ModeScientific = 201, CommandASIN = 202, CommandACOS = 203,
        CommandATAN = 204, CommandPOWE = 205, CommandASINH = 206, CommandACOSH = 207,
        CommandATANH = 208, ModeProgrammer = 209, CommandHex = 313, CommandDec = 314,
        CommandOct = 315, CommandBin = 316, CommandQword = 317, CommandDword = 318,
        CommandWord = 319, CommandByte = 320, CommandSEC = 400, CommandASEC = 401,
        CommandCSC = 402, CommandACSC = 403, CommandCOT = 404, CommandACOT = 405,
        CommandSECH = 406, CommandASECH = 407, CommandCSCH = 408, CommandACSCH = 409,
        CommandCOTH = 410, CommandACOTH = 411, CommandPOW2 = 412, CommandAbs = 413,
        CommandFloor = 414, CommandCeil = 415, CommandROLC = 416, CommandRORC = 417,
        CommandLogBaseY = 500, CommandNand = 501, CommandNor = 502, CommandRSHFL = 505,
        CommandRand = 600, CommandEuler = 601, CommandBINEDITSTART = 700, CommandBINEDITEND = 763
    }

    public enum CalculatorMode { Standard, Scientific }
    public enum CommandType { UnaryCommand, BinaryCommand, OperandCommand, Parentheses }

    public sealed class HistoryToken
    {
        public string Value { get; set; }
        public int CommandIndex { get; set; }
    }

    public sealed class ExpressionCommandWrapper
    {
        public ExpressionCommandWrapper() : this(CommandType.OperandCommand, 0, Array.Empty<int>(), false, false, false) { }
        public ExpressionCommandWrapper(CommandType type, int command, int[] commands, bool isNegative, bool isDecimalPresent, bool isSciFmt)
        {
            Type = type; Command = command; Commands = commands ?? Array.Empty<int>(); IsNegative = isNegative;
            IsDecimalPresent = isDecimalPresent; IsSciFmt = isSciFmt;
        }
        public CommandType Type { get; }
        public int Command { get; }
        public int[] Commands { get; }
        public bool IsNegative { get; }
        public bool IsDecimalPresent { get; }
        public bool IsSciFmt { get; }
    }

    public sealed class HistoryItemWrapper
    {
        public HistoryItemWrapper() : this(Array.Empty<HistoryToken>(), Array.Empty<ExpressionCommandWrapper>(), string.Empty, string.Empty) { }
        public HistoryItemWrapper(HistoryToken[] tokens, ExpressionCommandWrapper[] commands, string expression, string result)
        {
            Tokens = tokens ?? Array.Empty<HistoryToken>(); Commands = commands ?? Array.Empty<ExpressionCommandWrapper>();
            Expression = expression ?? string.Empty; Result = result ?? string.Empty;
        }
        public HistoryToken[] Tokens { get; }
        public ExpressionCommandWrapper[] Commands { get; }
        public string Expression { get; }
        public string Result { get; }
    }

    public delegate void SetPrimaryDisplayHandler(string display, bool isError);
    public delegate void SetIsInErrorHandler(bool isError);
    public delegate void SetExpressionDisplayHandler(HistoryToken[] tokens, ExpressionCommandWrapper[] commands);
    public delegate void SetParenthesisNumberHandler(uint count);
    public delegate void SimpleHandler();
    public delegate void OnHistoryItemAddedHandler(uint addedItemIndex);
    public delegate void SetMemorizedNumbersHandler(string[] memorizedNumbers);
    public delegate void MemoryItemChangedHandler(uint indexOfMemory);
    public delegate string GetCEngineStringHandler(string id);

    public sealed class CalculatorManagerWrapper
    {
        private readonly SetPrimaryDisplayHandler _display;
        private readonly SetIsInErrorHandler _error;
        private readonly SetExpressionDisplayHandler _expression;
        private readonly OnHistoryItemAddedHandler _historyAdded;
        private readonly SetMemorizedNumbersHandler _memoryDisplay;
        private readonly MemoryItemChangedHandler _memoryChanged;
        private readonly SimpleHandler _inputChanged;
        private readonly List<HistoryItemWrapper> _history = new List<HistoryItemWrapper>();
        private readonly List<double> _memory = new List<double>();
        private string _input = "0";
        private double _accumulator;
        private CalculatorCommand _pending = CalculatorCommand.CommandNULL;
        private bool _newInput = true;
        private bool _inverse;
        private int _radix = 10;

        public CalculatorManagerWrapper(SetPrimaryDisplayHandler onSetPrimaryDisplay, SetIsInErrorHandler onSetIsInError,
            SetExpressionDisplayHandler onSetExpressionDisplay, SetParenthesisNumberHandler onSetParenthesisNumber,
            SimpleHandler onNoRightParenAdded, SimpleHandler onMaxDigitsReached, SimpleHandler onBinaryOperatorReceived,
            OnHistoryItemAddedHandler onHistoryItemAdded, SetMemorizedNumbersHandler onSetMemorizedNumbers,
            MemoryItemChangedHandler onMemoryItemChanged, SimpleHandler onInputChanged, GetCEngineStringHandler onGetCEngineString)
        {
            _display = onSetPrimaryDisplay; _error = onSetIsInError; _expression = onSetExpressionDisplay;
            _historyAdded = onHistoryItemAdded; _memoryDisplay = onSetMemorizedNumbers;
            _memoryChanged = onMemoryItemChanged; _inputChanged = onInputChanged;
            Publish(false);
        }

        public bool IsEngineRecording => false;
        public bool IsInputEmpty => _newInput;
        public char DecimalSeparator => CultureInfo.CurrentCulture.NumberFormat.NumberDecimalSeparator[0];
        public ulong MaxHistorySize => 100;

        public void Reset(bool clearMemory)
        {
            _input = "0"; _accumulator = 0; _pending = CalculatorCommand.CommandNULL; _newInput = true;
            if (clearMemory) _memory.Clear();
            Publish(false); PublishMemory();
        }
        public void SetStandardMode() { _radix = 10; }
        public void SetScientificMode() { _radix = 10; }
        public void SetProgrammerMode() { }

        public void SendCommand(CalculatorCommand command)
        {
            int raw = (int)command;
            if (raw >= 130 && raw <= 145) { AppendDigit(raw <= 139 ? raw - 130 : raw - 140 + 10); return; }
            try
            {
                switch (command)
                {
                    case CalculatorCommand.CommandPNT: AppendDecimal(); break;
                    case CalculatorCommand.CommandBACK: Backspace(); break;
                    case CalculatorCommand.CommandCLEAR: case CalculatorCommand.CommandCENTR: Reset(false); break;
                    case CalculatorCommand.CommandSIGN: SetValue(-CurrentValue()); break;
                    case CalculatorCommand.CommandADD: case CalculatorCommand.CommandSUB:
                    case CalculatorCommand.CommandMUL: case CalculatorCommand.CommandDIV:
                    case CalculatorCommand.CommandMOD: case CalculatorCommand.CommandPWR:
                    case CalculatorCommand.CommandAnd: case CalculatorCommand.CommandOR:
                    case CalculatorCommand.CommandXor: case CalculatorCommand.CommandLSHF:
                    case CalculatorCommand.CommandRSHF: BeginBinary(command); break;
                    case CalculatorCommand.CommandEQU: Complete(); break;
                    case CalculatorCommand.CommandSQRT: Unary(Math.Sqrt); break;
                    case CalculatorCommand.CommandSQR: Unary(x => x * x); break;
                    case CalculatorCommand.CommandCUB: Unary(x => x * x * x); break;
                    case CalculatorCommand.CommandCUBEROOT: Unary(x => Math.Sign(x) * Math.Pow(Math.Abs(x), 1d / 3d)); break;
                    case CalculatorCommand.CommandREC: Unary(x => 1d / x); break;
                    case CalculatorCommand.CommandAbs: Unary(Math.Abs); break;
                    case CalculatorCommand.CommandFloor: Unary(Math.Floor); break;
                    case CalculatorCommand.CommandCeil: Unary(Math.Ceiling); break;
                    case CalculatorCommand.CommandSIN: Unary(x => _inverse ? Math.Asin(x) : Math.Sin(ToRadians(x))); break;
                    case CalculatorCommand.CommandCOS: Unary(x => _inverse ? Math.Acos(x) : Math.Cos(ToRadians(x))); break;
                    case CalculatorCommand.CommandTAN: Unary(x => _inverse ? Math.Atan(x) : Math.Tan(ToRadians(x))); break;
                    case CalculatorCommand.CommandASIN: Unary(Math.Asin); break;
                    case CalculatorCommand.CommandACOS: Unary(Math.Acos); break;
                    case CalculatorCommand.CommandATAN: Unary(Math.Atan); break;
                    case CalculatorCommand.CommandSINH: Unary(Math.Sinh); break;
                    case CalculatorCommand.CommandCOSH: Unary(Math.Cosh); break;
                    case CalculatorCommand.CommandTANH: Unary(Math.Tanh); break;
                    case CalculatorCommand.CommandLN: Unary(Math.Log); break;
                    case CalculatorCommand.CommandLOG: Unary(Math.Log10); break;
                    case CalculatorCommand.CommandPOW10: Unary(x => Math.Pow(10, x)); break;
                    case CalculatorCommand.CommandPOW2: Unary(x => Math.Pow(2, x)); break;
                    case CalculatorCommand.CommandPI: SetValue(Math.PI); break;
                    case CalculatorCommand.CommandEuler: SetValue(Math.E); break;
                    case CalculatorCommand.CommandPERCENT: SetValue(CurrentValue() / 100d); break;
                    case CalculatorCommand.CommandINV: _inverse = !_inverse; break;
                    case CalculatorCommand.CommandMCLEAR: MemorizedNumberClearAll(); break;
                    case CalculatorCommand.CommandRECALL: if (_memory.Count > 0) SetValue(_memory[0]); break;
                    case CalculatorCommand.CommandSTORE: MemorizeNumber(); break;
                    case CalculatorCommand.CommandMPLUS: EnsureMemory(); _memory[0] += CurrentValue(); PublishMemory(); break;
                    case CalculatorCommand.CommandMMINUS: EnsureMemory(); _memory[0] -= CurrentValue(); PublishMemory(); break;
                    case CalculatorCommand.CommandHex: SetRadix(16); break;
                    case CalculatorCommand.CommandDec: SetRadix(10); break;
                    case CalculatorCommand.CommandOct: SetRadix(8); break;
                    case CalculatorCommand.CommandBin: SetRadix(2); break;
                }
                _inputChanged?.Invoke();
            }
            catch { Publish(true); }
        }

        private void AppendDigit(int digit)
        {
            if (digit >= _radix) return;
            string value = digit < 10 ? digit.ToString() : ((char)('A' + digit - 10)).ToString();
            _input = _newInput || _input == "0" ? value : _input + value; _newInput = false; Publish(false); _inputChanged?.Invoke();
        }
        private void AppendDecimal()
        {
            if (_radix != 10) return;
            string separator = CultureInfo.CurrentCulture.NumberFormat.NumberDecimalSeparator;
            if (_newInput) { _input = "0" + separator; _newInput = false; }
            else if (!_input.Contains(separator)) _input += separator;
            Publish(false);
        }
        private void Backspace() { if (!_newInput && _input.Length > 1) _input = _input.Substring(0, _input.Length - 1); else _input = "0"; Publish(false); }
        private void BeginBinary(CalculatorCommand command) { if (_pending != CalculatorCommand.CommandNULL && !_newInput) ApplyPending(); else _accumulator = CurrentValue(); _pending = command; _newInput = true; }
        private void Complete()
        {
            if (_pending == CalculatorCommand.CommandNULL) return;
            string expression = Format(_accumulator) + " " + _pending + " " + _input;
            ApplyPending(); _pending = CalculatorCommand.CommandNULL; _newInput = true;
            var item = new HistoryItemWrapper(Array.Empty<HistoryToken>(), Array.Empty<ExpressionCommandWrapper>(), expression, _input);
            _history.Insert(0, item); if (_history.Count > 100) _history.RemoveAt(_history.Count - 1);
            _historyAdded?.Invoke(0); Publish(false);
        }
        private void ApplyPending()
        {
            double right = CurrentValue();
            switch (_pending)
            {
                case CalculatorCommand.CommandADD: _accumulator += right; break;
                case CalculatorCommand.CommandSUB: _accumulator -= right; break;
                case CalculatorCommand.CommandMUL: _accumulator *= right; break;
                case CalculatorCommand.CommandDIV: _accumulator /= right; break;
                case CalculatorCommand.CommandMOD: _accumulator %= right; break;
                case CalculatorCommand.CommandPWR: _accumulator = Math.Pow(_accumulator, right); break;
                case CalculatorCommand.CommandAnd: _accumulator = (long)_accumulator & (long)right; break;
                case CalculatorCommand.CommandOR: _accumulator = (long)_accumulator | (long)right; break;
                case CalculatorCommand.CommandXor: _accumulator = (long)_accumulator ^ (long)right; break;
                case CalculatorCommand.CommandLSHF: _accumulator = (long)_accumulator << (int)right; break;
                case CalculatorCommand.CommandRSHF: _accumulator = (long)_accumulator >> (int)right; break;
            }
            SetValue(_accumulator);
        }
        private void Unary(Func<double, double> operation) { SetValue(operation(CurrentValue())); _newInput = true; }
        private double ToRadians(double value) => value * Math.PI / 180d;
        private double CurrentValue()
        {
            if (_radix == 10) return double.Parse(_input, NumberStyles.Float, CultureInfo.CurrentCulture);
            return Convert.ToInt64(_input, _radix);
        }
        private void SetValue(double value) { _input = _radix == 10 ? Format(value) : Convert.ToString((long)value, _radix).ToUpperInvariant(); Publish(false); }
        private static string Format(double value) => value.ToString("G15", CultureInfo.InvariantCulture);
        private void Publish(bool isError) { _display?.Invoke(isError ? "Error" : _input, isError); _error?.Invoke(isError); _expression?.Invoke(Array.Empty<HistoryToken>(), Array.Empty<ExpressionCommandWrapper>()); }
        private void EnsureMemory() { if (_memory.Count == 0) _memory.Add(0); }
        private void PublishMemory() => _memoryDisplay?.Invoke(_memory.Select(Format).ToArray());

        public void MemorizeNumber() { _memory.Insert(0, CurrentValue()); PublishMemory(); }
        public void MemorizedNumberLoad(uint index) { if (index < _memory.Count) SetValue(_memory[(int)index]); }
        public void MemorizedNumberAdd(uint index) { if (index < _memory.Count) { _memory[(int)index] += CurrentValue(); PublishMemory(); _memoryChanged?.Invoke(index); } }
        public void MemorizedNumberSubtract(uint index) { if (index < _memory.Count) { _memory[(int)index] -= CurrentValue(); PublishMemory(); _memoryChanged?.Invoke(index); } }
        public void MemorizedNumberClear(uint index) { if (index < _memory.Count) { _memory.RemoveAt((int)index); PublishMemory(); } }
        public void MemorizedNumberClearAll() { _memory.Clear(); PublishMemory(); }
        public void SetRadix(int radixType) { _radix = radixType == 0 ? 16 : radixType == 1 ? 10 : radixType == 2 ? 8 : radixType == 3 ? 2 : radixType; if (_radix < 2 || _radix > 16) _radix = 10; SetValue(CurrentValue()); }
        public void SetMemorizedNumbersString() => PublishMemory();
        public string GetResultForRadix(uint radix, int precision, bool groupDigitsPerRadix) { long value = (long)CurrentValue(); return Convert.ToString(value, (int)radix).ToUpperInvariant(); }
        public void SetPrecision(int precision) { }
        public void UpdateMaxIntDigits() { }
        public HistoryItemWrapper[] GetHistoryItems() => _history.ToArray();
        public HistoryItemWrapper[] GetHistoryItemsForMode(CalculatorMode mode) => GetHistoryItems();
        public void SetHistoryItems(HistoryItemWrapper[] historyItems) { _history.Clear(); if (historyItems != null) _history.AddRange(historyItems); }
        public HistoryItemWrapper GetHistoryItem(uint index) => index < _history.Count ? _history[(int)index] : null;
        public bool RemoveHistoryItem(uint index) { if (index >= _history.Count) return false; _history.RemoveAt((int)index); return true; }
        public void ClearHistory() => _history.Clear();
        public CalculatorCommand GetCurrentDegreeMode() => CalculatorCommand.CommandNULL;
        public void SetInHistoryItemLoadMode(bool isHistoryItemLoadMode) { }
        public ExpressionCommandWrapper[] GetDisplayCommandsSnapshot() => Array.Empty<ExpressionCommandWrapper>();
    }

    public enum UnitConverterCommand { Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine, Decimal, Negate, Backspace, Clear, Reset, None }
    public struct UnitWrapper { public int Id; public string Name; public string AccessibleName; public string Abbreviation; public bool IsConversionSource; public bool IsConversionTarget; public bool IsWhimsical; }
    public struct CategoryWrapper { public int Id; public string Name; public bool SupportsNegative; }
    public struct ConversionDataWrapper { public double Ratio; public double Offset; public bool OffsetFirst; }
    public struct UnitConversionEntry { public UnitWrapper Unit; public double Ratio; public double Offset; public bool OffsetFirst; }
    public struct SuggestedValueWrapper { public string Value; public UnitWrapper Unit; }
    public sealed class CategorySelectionResult { public UnitWrapper[] Units { get; internal set; } = Array.Empty<UnitWrapper>(); public UnitWrapper FromUnit { get; internal set; } public UnitWrapper ToUnit { get; internal set; } }

    public class UnitConverterVMCallbackBase
    {
        protected virtual void DisplayCallback(string fromValue, string toValue) { }
        protected virtual void SuggestedValueCallback(SuggestedValueWrapper[] suggestedValues) { }
        protected virtual void MaxDigitsReached() { }
        internal void Display(string fromValue, string toValue) => DisplayCallback(fromValue, toValue);
        internal void Suggest(SuggestedValueWrapper[] values) => SuggestedValueCallback(values);
    }
    public class ViewModelCurrencyCallbackBase
    {
        protected virtual void CurrencyDataLoadFinished(bool didLoad) { }
        protected virtual void CurrencySymbolsCallback(string fromSymbol, string toSymbol) { }
        protected virtual void CurrencyRatiosCallback(string ratioEquality, string accRatioEquality) { }
        protected virtual void CurrencyTimestampCallback(string timestamp, bool isWeekOldData) { }
        protected virtual void NetworkBehaviorChanged(int newBehavior) { }
    }
    public class ConverterDataLoaderBase
    {
        protected virtual void LoadData() { }
        protected virtual CategoryWrapper[] GetOrderedCategories() => Array.Empty<CategoryWrapper>();
        protected virtual UnitWrapper[] GetOrderedUnits(CategoryWrapper category) => Array.Empty<UnitWrapper>();
        protected virtual UnitConversionEntry[] LoadOrderedRatios(UnitWrapper unit) => Array.Empty<UnitConversionEntry>();
        protected virtual bool SupportsCategory(CategoryWrapper target) => true;
        internal void Load() => LoadData();
        internal CategoryWrapper[] Categories() => GetOrderedCategories();
        internal UnitWrapper[] Units(CategoryWrapper category) => GetOrderedUnits(category);
        internal UnitConversionEntry[] Ratios(UnitWrapper unit) => LoadOrderedRatios(unit);
    }

    public sealed class UnitConverterWrapper
    {
        private readonly ConverterDataLoaderBase _loader;
        private CategoryWrapper[] _categories = Array.Empty<CategoryWrapper>();
        private CategoryWrapper _category;
        private UnitWrapper[] _units = Array.Empty<UnitWrapper>();
        private UnitWrapper _from;
        private UnitWrapper _to;
        private UnitConverterVMCallbackBase _callback;
        private string _input = "0";
        private bool _switched;
        public UnitConverterWrapper(ConverterDataLoaderBase dataLoader) { _loader = dataLoader; }
        public bool IsSwitchedActive => _switched;
        public void Initialize() { _loader.Load(); _categories = _loader.Categories(); if (_categories.Length > 0) SetCurrentCategory(_categories[0]); }
        public CategoryWrapper[] GetCategories() => _categories;
        public CategorySelectionResult SetCurrentCategory(CategoryWrapper category)
        {
            _category = category; _units = _loader.Units(category);
            _from = _units.FirstOrDefault(x => x.IsConversionSource); if (_from.Name == null && _units.Length > 0) _from = _units[0];
            _to = _units.FirstOrDefault(x => x.IsConversionTarget); if (_to.Name == null && _units.Length > 1) _to = _units[1]; else if (_to.Name == null) _to = _from;
            Calculate(); return new CategorySelectionResult { Units = _units, FromUnit = _from, ToUnit = _to };
        }
        public CategoryWrapper GetCurrentCategory() => _category;
        public void SetCurrentUnitTypes(UnitWrapper fromType, UnitWrapper toType) { _from = fromType; _to = toType; Calculate(); }
        public void SwitchActive(string newValue) { var temp = _from; _from = _to; _to = temp; _input = string.IsNullOrWhiteSpace(newValue) ? "0" : newValue; _switched = !_switched; Calculate(); }
        public string SaveUserPreferences() => _category.Id + ";" + _from.Id + ";" + _to.Id;
        public void RestoreUserPreferences(string userPreference) { }
        public void SendCommand(UnitConverterCommand command)
        {
            if (command >= UnitConverterCommand.Zero && command <= UnitConverterCommand.Nine) _input = _input == "0" ? ((int)command).ToString() : _input + (int)command;
            else if (command == UnitConverterCommand.Decimal && !_input.Contains(".")) _input += ".";
            else if (command == UnitConverterCommand.Backspace) _input = _input.Length > 1 ? _input.Substring(0, _input.Length - 1) : "0";
            else if (command == UnitConverterCommand.Negate) _input = _input.StartsWith("-") ? _input.Substring(1) : "-" + _input;
            else if (command == UnitConverterCommand.Clear || command == UnitConverterCommand.Reset) _input = "0";
            Calculate();
        }
        public void SetViewModelCallback(UnitConverterVMCallbackBase callback) { _callback = callback; Calculate(); }
        public void SetViewModelCurrencyCallback(ViewModelCurrencyCallbackBase callback) { }
        public void Calculate()
        {
            if (_callback == null) return;
            double.TryParse(_input, NumberStyles.Float, CultureInfo.InvariantCulture, out double value);
            UnitConversionEntry fromEntry = FindRatio(_from, _from);
            UnitConversionEntry toEntry = FindRatio(_from, _to);
            double baseValue = fromEntry.OffsetFirst ? (value + fromEntry.Offset) * fromEntry.Ratio : value * fromEntry.Ratio + fromEntry.Offset;
            double result = toEntry.OffsetFirst ? baseValue / toEntry.Ratio - toEntry.Offset : (baseValue - toEntry.Offset) / (toEntry.Ratio == 0 ? 1 : toEntry.Ratio);
            _callback.Display(_input, result.ToString("G15", CultureInfo.CurrentCulture));
        }
        private UnitConversionEntry FindRatio(UnitWrapper source, UnitWrapper target)
        {
            var entries = _loader.Ratios(source);
            foreach (var entry in entries) if (entry.Unit.Id == target.Id) return entry;
            return new UnitConversionEntry { Unit = target, Ratio = 1 };
        }
        public void ResetCategoriesAndRatios() => Initialize();
    }
}
