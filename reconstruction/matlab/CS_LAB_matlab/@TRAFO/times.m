function out = times( a,b )
%TIMES (element-wise) for nested cells
%
% (c) Thomas Kuestner 
% ---------------------------------------------------------------------

% determine input types
inType = {class(a), class(b)};
if(strcmp(inType{1},'TRAFO') && strcmp(inType{2},'TRAFO'))  
    [outA, idx] = flattenCellMatrix(a.data);
    [outB, idxB] = flattenCellMatrix(b.data);
    meta = a.meta;
    meta_b = b.meta;
    clear 'a' 'b'

    if(~isempty(idx) && ~isempty(idxB) && ~all(cellfun(@(x,y) isequal(x,y), idx, idxB)))
        error('TRAFO::times: Nested cells must have the same size');
    end
    if (iscell(meta) && iscell(meta_b))
        if(~isempty(meta) && ~isempty(meta_b))
            if(iscell(meta{1}) && iscell(meta_b{1}))
                if(~all(cellfun(@(x,y) isequal(x,y), flattenCellMatrix(meta), flattenCellMatrix(meta_b))))
                    error('TRAFO::times: Unequal reconstruction information');
                end
            else
                if(~all(cellfun(@(x,y) isequal(x,y), meta, meta_b)))
                    % happens in kernelMult for kernel .* zpad(W) -> take kernel
                    % meta information (meta) => corrected
                    [ST, ~] = dbstack;
                    if(~strcmp(ST(2,1).name,'kernelMult'))
                        error('TRAFO::times: Unequal reconstruction information');
                    end
                end
            end
        end
    else 
        if ~isequal(meta, meta_b)
            error('TRAFO::times: Unequal reconstruction information');
        end
    end
    
%     try
        out = cellfun(@(x,y) x.*y, outA, outB, 'UniformOutput', false);
%     catch msg
%         out = cell(size(outA));
%         for i=1:length(out)
%             out{i} = outA{i} .* outB{i};
%         end
%     end

elseif(strcmp(inType{1},'TRAFO')) % assume b is double/uint/int
    [out, idx] = flattenCellMatrix(a.data);
    meta = a.meta;
    clear 'a'
    
    out = cellfun(@(x) x .* b, out, 'UniformOutput', false);
    
elseif(strcmp(inType{2},'TRAFO'))
    [out, idx] = flattenCellMatrix(b.data);
    meta = b.meta;
    clear 'b'
    
    out = cellfun(@(x) a .* x, out, 'UniformOutput', false);
    
else
    error('TRAFO::times: Impossible constellation!');
end

out = reconFlatCellMatrix(out,idx);
out = TRAFO(out,meta); % return TRAFO object again without modifying input

end

