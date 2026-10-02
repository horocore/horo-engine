import type { InputHTMLAttributes, ReactNode, SelectHTMLAttributes } from 'react';

type FieldProps = { label: string; hint?: string; children: ReactNode };

export function Field({ label, hint, children }: FieldProps) {
  return <label className="release-field"><span>{label}</span>{children}{hint && <small>{hint}</small>}</label>;
}

export function TextField(props: Omit<FieldProps, 'children'> & InputHTMLAttributes<HTMLInputElement>) {
  const { label, hint, ...inputProps } = props;
  return <Field label={label} hint={hint}><input {...inputProps} /></Field>;
}

export function SelectField(props: FieldProps & SelectHTMLAttributes<HTMLSelectElement>) {
  const { label, hint, children, ...selectProps } = props;
  return <Field label={label} hint={hint}><select {...selectProps}>{children}</select></Field>;
}
