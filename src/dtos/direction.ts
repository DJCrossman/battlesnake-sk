export enum DirectionEnum {
  'up' = 'up',
  'down' = 'down',
  'left' = 'left',
  'right' = 'right',
}
export type Direction = keyof typeof DirectionEnum